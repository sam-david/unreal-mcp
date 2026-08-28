#include "MCPBridgeCommands.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "SubobjectData.h"
#include "SubobjectDataHandle.h"
#include "SubobjectDataSubsystem.h"
#include "UnrealMCPBridgeModule.h"

namespace
{
	// ---------------------------------------------------------------------
	// Response helpers
	// ---------------------------------------------------------------------

	TSharedRef<FJsonObject> MakeFailure(const FString& Message)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetBoolField(TEXT("success"), false);
		Response->SetStringField(TEXT("error"), Message);
		return Response;
	}

	TSharedRef<FJsonObject> MakeSuccess(const TSharedRef<FJsonObject>& Data)
	{
		const TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetBoolField(TEXT("success"), true);
		Response->SetObjectField(TEXT("data"), Data);
		return Response;
	}

	FString GetStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
		const FString& Fallback = FString())
	{
		FString Value;
		if (Params.IsValid() && Params->TryGetStringField(Field, Value) && !Value.IsEmpty())
		{
			return Value;
		}
		return Fallback;
	}

	// ---------------------------------------------------------------------
	// Lookup helpers
	// ---------------------------------------------------------------------

	/** Accepts a short class name ("Actor"), a /Script path, or a Blueprint asset path. */
	UClass* ResolveClass(const FString& InName)
	{
		if (InName.IsEmpty())
		{
			return nullptr;
		}

		if (InName.Contains(TEXT("/")))
		{
			if (UClass* Direct = LoadObject<UClass>(nullptr, *InName))
			{
				return Direct;
			}
			// A Blueprint asset path was given: use the class it generates.
			if (const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *InName))
			{
				return Blueprint->GeneratedClass;
			}
			return nullptr;
		}

		return UClass::TryFindTypeSlow<UClass>(InName);
	}

	UBlueprint* LoadBlueprintParam(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		const FString Path = GetStringParam(Params, TEXT("blueprint_path"));
		if (Path.IsEmpty())
		{
			OutError = TEXT("Missing required parameter: blueprint_path");
			return nullptr;
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
		if (Blueprint == nullptr)
		{
			OutError = FString::Printf(TEXT("Blueprint not found: %s"), *Path);
		}
		return Blueprint;
	}

	/** The event graph is the first ubergraph page; Blueprints always have one. */
	UEdGraph* FindEventGraph(UBlueprint* Blueprint)
	{
		return Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FString& NodeId, UEdGraph*& OutGraph)
	{
		FGuid Guid;
		if (!FGuid::Parse(NodeId, Guid))
		{
			return nullptr;
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->NodeGuid == Guid)
				{
					OutGraph = Graph;
					return Node;
				}
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->Direction == Direction
				&& Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FString ListPinNames(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		TArray<FString> Names;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->Direction == Direction)
			{
				Names.Add(Pin->PinName.ToString());
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	// ---------------------------------------------------------------------
	// Serialization helpers
	// ---------------------------------------------------------------------

	TSharedRef<FJsonObject> DescribePin(UEdGraphPin* Pin)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Json->SetStringField(TEXT("direction"),
			Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Json->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
		Json->SetNumberField(TEXT("connections"), Pin->LinkedTo.Num());
		return Json;
	}

	TSharedRef<FJsonObject> DescribeNode(UEdGraphNode* Node)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
		Json->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Json->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Json->SetNumberField(TEXT("x"), Node->NodePosX);
		Json->SetNumberField(TEXT("y"), Node->NodePosY);

		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr)
			{
				Pins.Add(MakeShared<FJsonValueObject>(DescribePin(Pin)));
			}
		}
		Json->SetArrayField(TEXT("pins"), Pins);
		return Json;
	}

	// ---------------------------------------------------------------------
	// Mutation helpers
	// ---------------------------------------------------------------------

	void CompileAndSave(UBlueprint* Blueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);

		if (UPackage* Package = Blueprint->GetOutermost())
		{
			Package->MarkPackageDirty();
			UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ false);
		}
	}

	/**
	 * Creates and registers a node. Configure runs before AllocateDefaultPins
	 * because nodes like UK2Node_CallFunction derive their pins from state that
	 * has to be set first.
	 */
	template <typename TNodeType>
	TNodeType* AllocateNode(UEdGraph* Graph, int32 PosX, int32 PosY,
		TFunctionRef<void(TNodeType*)> Configure)
	{
		TNodeType* Node = NewObject<TNodeType>(Graph);
		Configure(Node);

		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();

		Node->NodePosX = PosX;
		Node->NodePosY = PosY;

		Graph->AddNode(Node, /*bUserAction*/ false, /*bSelectNewNode*/ false);
		return Node;
	}

	bool BuildPinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
	{
		OutType = FEdGraphPinType();
		const FString Normalized = TypeName.ToLower();

		if (Normalized == TEXT("bool"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (Normalized == TEXT("int"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (Normalized == TEXT("float"))
		{
			// Blueprint "float" is a double-backed real since UE5.
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		}
		else if (Normalized == TEXT("string"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (Normalized == TEXT("vector"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		}
		else if (Normalized == TEXT("rotator"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
		}
		else if (Normalized == TEXT("object"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = UObject::StaticClass();
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unsupported variable_type '%s'. Expected one of: ")
				TEXT("bool, int, float, string, vector, rotator, object."), *TypeName);
			return false;
		}

		return true;
	}

	// ---------------------------------------------------------------------
	// Node construction
	// ---------------------------------------------------------------------

	/** Resolves target_class + function_name into a UFunction for call-style nodes. */
	UFunction* ResolveFunction(const TSharedPtr<FJsonObject>& Props, const FString& DefaultClass,
		const FString& DefaultFunction, FString& OutError)
	{
		const FString ClassName = GetStringParam(Props, TEXT("target_class"), DefaultClass);
		const FString FunctionName = GetStringParam(Props, TEXT("function_name"), DefaultFunction);

		if (FunctionName.IsEmpty())
		{
			OutError = TEXT("Missing property: function_name");
			return nullptr;
		}

		UClass* TargetClass = ResolveClass(ClassName);
		if (TargetClass == nullptr)
		{
			OutError = FString::Printf(TEXT("Class not found: %s"), *ClassName);
			return nullptr;
		}

		UFunction* Function = TargetClass->FindFunctionByName(FName(*FunctionName));
		if (Function == nullptr)
		{
			OutError = FString::Printf(TEXT("Function '%s' not found on class '%s'"),
				*FunctionName, *ClassName);
		}
		return Function;
	}

	UEdGraphNode* CreateNodeOfType(UBlueprint* Blueprint, UEdGraph* Graph, const FString& NodeType,
		const TSharedPtr<FJsonObject>& Props, int32 PosX, int32 PosY, FString& OutError)
	{
		const FString Type = NodeType.ToLower();

		if (Type == TEXT("branch") || Type == TEXT("ifthenelse"))
		{
			return AllocateNode<UK2Node_IfThenElse>(Graph, PosX, PosY, [](UK2Node_IfThenElse*) {});
		}

		if (Type == TEXT("executionsequence") || Type == TEXT("sequence"))
		{
			return AllocateNode<UK2Node_ExecutionSequence>(Graph, PosX, PosY,
				[](UK2Node_ExecutionSequence*) {});
		}

		if (Type == TEXT("print") || Type == TEXT("printstring"))
		{
			UFunction* Function = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
				GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString));
			if (Function == nullptr)
			{
				OutError = TEXT("Could not resolve UKismetSystemLibrary::PrintString");
				return nullptr;
			}
			return AllocateNode<UK2Node_CallFunction>(Graph, PosX, PosY,
				[Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
		}

		if (Type == TEXT("callfunction"))
		{
			UFunction* Function = ResolveFunction(Props, TEXT("KismetSystemLibrary"), FString(), OutError);
			if (Function == nullptr)
			{
				return nullptr;
			}
			return AllocateNode<UK2Node_CallFunction>(Graph, PosX, PosY,
				[Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
		}

		if (Type == TEXT("comparison"))
		{
			UFunction* Function = ResolveFunction(Props, TEXT("KismetMathLibrary"),
				TEXT("Greater_IntInt"), OutError);
			if (Function == nullptr)
			{
				return nullptr;
			}
			return AllocateNode<UK2Node_CallFunction>(Graph, PosX, PosY,
				[Function](UK2Node_CallFunction* Node) { Node->SetFromFunction(Function); });
		}

		if (Type == TEXT("variableget") || Type == TEXT("variableset"))
		{
			const FString VariableName = GetStringParam(Props, TEXT("variable_name"));
			if (VariableName.IsEmpty())
			{
				OutError = TEXT("Missing property: variable_name");
				return nullptr;
			}

			if (Type == TEXT("variableget"))
			{
				return AllocateNode<UK2Node_VariableGet>(Graph, PosX, PosY,
					[&VariableName](UK2Node_VariableGet* Node)
					{
						Node->VariableReference.SetSelfMember(FName(*VariableName));
					});
			}

			return AllocateNode<UK2Node_VariableSet>(Graph, PosX, PosY,
				[&VariableName](UK2Node_VariableSet* Node)
				{
					Node->VariableReference.SetSelfMember(FName(*VariableName));
				});
		}

		if (Type == TEXT("receivebeginplay") || Type == TEXT("receivetick") || Type == TEXT("event"))
		{
			FString EventName = GetStringParam(Props, TEXT("event_name"));
			if (EventName.IsEmpty())
			{
				EventName = (Type == TEXT("receivetick")) ? TEXT("ReceiveTick") : TEXT("ReceiveBeginPlay");
			}

			const FString OwnerClassName = GetStringParam(Props, TEXT("parent_class"), TEXT("Actor"));
			UClass* OwnerClass = ResolveClass(OwnerClassName);
			if (OwnerClass == nullptr)
			{
				OutError = FString::Printf(TEXT("Class not found: %s"), *OwnerClassName);
				return nullptr;
			}

			if (OwnerClass->FindFunctionByName(FName(*EventName)) == nullptr)
			{
				OutError = FString::Printf(TEXT("Event '%s' not found on class '%s'"),
					*EventName, *OwnerClassName);
				return nullptr;
			}

			// Reuse an existing override instead of adding a second one, which is
			// what the editor does: two enabled Event BeginPlay nodes fail to
			// compile, and the Actor template already ships ghosted BeginPlay,
			// Tick and ActorBeginOverlap nodes that callers mean to target.
			if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
					Blueprint, OwnerClass, FName(*EventName)))
			{
				Existing->SetEnabledState(ENodeEnabledState::Enabled, /*bUserAction*/ false);
				return Existing;
			}

			return AllocateNode<UK2Node_Event>(Graph, PosX, PosY,
				[&EventName, OwnerClass](UK2Node_Event* Node)
				{
					Node->EventReference.SetExternalMember(FName(*EventName), OwnerClass);
					Node->bOverrideFunction = true;
				});
		}

		if (Type == TEXT("dynamiccast") || Type == TEXT("cast"))
		{
			const FString TargetClassName = GetStringParam(Props, TEXT("target_class"));
			UClass* TargetClass = ResolveClass(TargetClassName);
			if (TargetClass == nullptr)
			{
				OutError = FString::Printf(
					TEXT("DynamicCast needs a resolvable target_class (got '%s')"), *TargetClassName);
				return nullptr;
			}
			return AllocateNode<UK2Node_DynamicCast>(Graph, PosX, PosY,
				[TargetClass](UK2Node_DynamicCast* Node)
				{
					Node->TargetType = TargetClass;
					Node->SetPurity(false);
				});
		}

		if (Type == TEXT("spawnactor") || Type == TEXT("spawnactorfromclass"))
		{
			return AllocateNode<UK2Node_SpawnActorFromClass>(Graph, PosX, PosY,
				[](UK2Node_SpawnActorFromClass*) {});
		}

		if (Type == TEXT("switch"))
		{
			const FString SwitchType = GetStringParam(Props, TEXT("switch_type"), TEXT("int")).ToLower();
			if (SwitchType == TEXT("string"))
			{
				return AllocateNode<UK2Node_SwitchString>(Graph, PosX, PosY, [](UK2Node_SwitchString*) {});
			}
			return AllocateNode<UK2Node_SwitchInteger>(Graph, PosX, PosY, [](UK2Node_SwitchInteger*) {});
		}

		OutError = FString::Printf(
			TEXT("Unknown node_type '%s'. Supported: Branch, ExecutionSequence, Print, CallFunction, ")
			TEXT("Comparison, VariableGet, VariableSet, ReceiveBeginPlay, ReceiveTick, Event, ")
			TEXT("DynamicCast, SpawnActor, Switch."), *NodeType);
		return nullptr;
	}

	// ---------------------------------------------------------------------
	// Commands
	// ---------------------------------------------------------------------

	TSharedRef<FJsonObject> CommandGetCapabilities()
	{
		static const TCHAR* SupportedCommands[] = {
			TEXT("get_capabilities"),
			TEXT("create_blueprint"),
			TEXT("add_component"),
			TEXT("add_variable"),
			TEXT("add_function"),
			TEXT("add_node"),
			TEXT("connect_nodes"),
			TEXT("remove_node"),
			TEXT("list_nodes"),
		};

		static const TCHAR* SupportedFeatures[] = {
			TEXT("blueprint_graph"),
			TEXT("node_editing"),
			TEXT("components"),
			TEXT("variables"),
			TEXT("functions"),
		};

		TArray<TSharedPtr<FJsonValue>> Commands;
		for (const TCHAR* Name : SupportedCommands)
		{
			Commands.Add(MakeShared<FJsonValueString>(Name));
		}

		TArray<TSharedPtr<FJsonValue>> Features;
		for (const TCHAR* Name : SupportedFeatures)
		{
			Features.Add(MakeShared<FJsonValueString>(Name));
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("version"), TEXT("1.0.0"));
		Data->SetArrayField(TEXT("commands"), Commands);
		Data->SetArrayField(TEXT("features"), Features);
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Name = GetStringParam(Params, TEXT("name"));
		if (Name.IsEmpty())
		{
			return MakeFailure(TEXT("Missing required parameter: name"));
		}

		const FString ParentClassName = GetStringParam(Params, TEXT("parent_class"), TEXT("Actor"));
		const FString Path = GetStringParam(Params, TEXT("path"), TEXT("/Game/Blueprints"));

		UClass* ParentClass = ResolveClass(ParentClassName);
		if (ParentClass == nullptr)
		{
			return MakeFailure(FString::Printf(TEXT("Parent class not found: %s"), *ParentClassName));
		}
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			return MakeFailure(FString::Printf(
				TEXT("Cannot create a Blueprint of class '%s'"), *ParentClassName));
		}

		const FString PackageName = Path / Name;
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return MakeFailure(FString::Printf(TEXT("An asset already exists at %s"), *PackageName));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (Package == nullptr)
		{
			return MakeFailure(FString::Printf(TEXT("Could not create package %s"), *PackageName));
		}

		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());

		if (Blueprint == nullptr)
		{
			return MakeFailure(TEXT("CreateBlueprint returned null"));
		}

		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
		UEditorLoadingAndSavingUtils::SavePackages({ Package }, /*bOnlyDirty*/ false);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("name"), Blueprint->GetName());
		Data->SetStringField(TEXT("path"), Blueprint->GetPathName());
		Data->SetStringField(TEXT("parent_class"), ParentClass->GetName());
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandAddComponent(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString ComponentClassName = GetStringParam(Params, TEXT("component_class"));
		UClass* ComponentClass = ResolveClass(ComponentClassName);
		if (ComponentClass == nullptr || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return MakeFailure(FString::Printf(
				TEXT("Not an actor component class: %s"), *ComponentClassName));
		}

		USubobjectDataSubsystem* Subsystem = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
		if (Subsystem == nullptr)
		{
			return MakeFailure(TEXT("SubobjectDataSubsystem unavailable"));
		}

		TArray<FSubobjectDataHandle> Handles;
		Subsystem->K2_GatherSubobjectDataForBlueprint(Blueprint, Handles);
		if (Handles.Num() == 0)
		{
			return MakeFailure(TEXT("Blueprint has no root subobject to attach to"));
		}

		FAddNewSubobjectParams AddParams;
		AddParams.ParentHandle = Handles[0];
		AddParams.NewClass = ComponentClass;
		AddParams.BlueprintContext = Blueprint;

		FText FailReason;
		const FSubobjectDataHandle NewHandle = Subsystem->AddNewSubobject(AddParams, FailReason);
		if (!NewHandle.IsValid())
		{
			return MakeFailure(FString::Printf(TEXT("Failed to add component: %s"),
				*FailReason.ToString()));
		}

		const FString RequestedName = GetStringParam(Params, TEXT("component_name"));
		if (!RequestedName.IsEmpty())
		{
			Subsystem->RenameSubobject(NewHandle, FText::FromString(RequestedName));
		}

		// Report the name the editor actually assigned, read before the compile
		// can invalidate the handle. With no component_name the editor derives one
		// from the class -- SphereComponent becomes "Sphere" -- and a requested
		// name may be adjusted to stay unique. Echoing the class name instead
		// would hand callers an identifier the Blueprint does not contain.
		FString ActualName;
		if (const FSubobjectData* SubobjectData = NewHandle.GetData())
		{
			ActualName = SubobjectData->GetVariableName().ToString();
		}
		if (ActualName.IsEmpty())
		{
			ActualName = RequestedName.IsEmpty() ? ComponentClass->GetName() : RequestedName;
		}

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("component"), ActualName);
		Data->SetStringField(TEXT("component_class"), ComponentClass->GetName());
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandAddVariable(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString VariableName = GetStringParam(Params, TEXT("variable_name"));
		if (VariableName.IsEmpty())
		{
			return MakeFailure(TEXT("Missing required parameter: variable_name"));
		}

		FEdGraphPinType PinType;
		if (!BuildPinType(GetStringParam(Params, TEXT("variable_type"), TEXT("bool")), PinType, Error))
		{
			return MakeFailure(Error);
		}

		const FString DefaultValue = GetStringParam(Params, TEXT("default_value"));
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VariableName), PinType, DefaultValue))
		{
			return MakeFailure(FString::Printf(
				TEXT("Could not add variable '%s' (a member with that name may already exist)"),
				*VariableName));
		}

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("variable"), VariableName);
		Data->SetStringField(TEXT("type"), PinType.PinCategory.ToString());
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandAddFunction(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString FunctionName = GetStringParam(Params, TEXT("function_name"));
		if (FunctionName.IsEmpty())
		{
			return MakeFailure(TEXT("Missing required parameter: function_name"));
		}

		for (const UEdGraph* Existing : Blueprint->FunctionGraphs)
		{
			if (Existing->GetName().Equals(FunctionName, ESearchCase::IgnoreCase))
			{
				return MakeFailure(FString::Printf(
					TEXT("Function '%s' already exists"), *FunctionName));
			}
		}

		UEdGraph* FunctionGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (FunctionGraph == nullptr)
		{
			return MakeFailure(TEXT("CreateNewGraph returned null"));
		}

		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			Blueprint, FunctionGraph, /*bIsUserCreated*/ true, nullptr);

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("function"), FunctionName);
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandAddNode(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString NodeType = GetStringParam(Params, TEXT("node_type"));
		if (NodeType.IsEmpty())
		{
			return MakeFailure(TEXT("Missing required parameter: node_type"));
		}

		// A named graph lets callers target a function graph instead of the event graph.
		UEdGraph* Graph = nullptr;
		const FString GraphName = GetStringParam(Params, TEXT("graph_name"));
		if (GraphName.IsEmpty())
		{
			Graph = FindEventGraph(Blueprint);
		}
		else
		{
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Candidate : Graphs)
			{
				if (Candidate->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					Graph = Candidate;
					break;
				}
			}
		}

		if (Graph == nullptr)
		{
			return MakeFailure(GraphName.IsEmpty()
				? TEXT("Blueprint has no event graph")
				: FString::Printf(TEXT("Graph not found: %s"), *GraphName));
		}

		double PosX = 0.0;
		double PosY = 0.0;
		Params->TryGetNumberField(TEXT("x"), PosX);
		Params->TryGetNumberField(TEXT("y"), PosY);

		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		Params->TryGetObjectField(TEXT("properties"), PropsPtr);
		const TSharedPtr<FJsonObject> Props =
			(PropsPtr != nullptr && PropsPtr->IsValid()) ? *PropsPtr : MakeShared<FJsonObject>();

		UEdGraphNode* Node = CreateNodeOfType(Blueprint, Graph, NodeType, Props,
			static_cast<int32>(PosX), static_cast<int32>(PosY), Error);
		if (Node == nullptr)
		{
			return MakeFailure(Error);
		}

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = DescribeNode(Node);
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
		Data->SetStringField(TEXT("graph"), Graph->GetName());
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandConnectNodes(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString SourceId = GetStringParam(Params, TEXT("source_node_id"));
		const FString TargetId = GetStringParam(Params, TEXT("target_node_id"));
		const FString SourcePinName = GetStringParam(Params, TEXT("source_pin"));
		const FString TargetPinName = GetStringParam(Params, TEXT("target_pin"));

		if (SourceId.IsEmpty() || TargetId.IsEmpty() || SourcePinName.IsEmpty() || TargetPinName.IsEmpty())
		{
			return MakeFailure(TEXT("source_node_id, source_pin, target_node_id and target_pin are all required"));
		}

		UEdGraph* SourceGraph = nullptr;
		UEdGraph* TargetGraph = nullptr;
		UEdGraphNode* SourceNode = FindNodeByGuid(Blueprint, SourceId, SourceGraph);
		UEdGraphNode* TargetNode = FindNodeByGuid(Blueprint, TargetId, TargetGraph);

		if (SourceNode == nullptr)
		{
			return MakeFailure(FString::Printf(TEXT("Source node not found: %s"), *SourceId));
		}
		if (TargetNode == nullptr)
		{
			return MakeFailure(FString::Printf(TEXT("Target node not found: %s"), *TargetId));
		}
		if (SourceGraph != TargetGraph)
		{
			return MakeFailure(TEXT("Cannot wire nodes that live in different graphs"));
		}

		UEdGraphPin* SourcePin = FindPin(SourceNode, SourcePinName, EGPD_Output);
		if (SourcePin == nullptr)
		{
			return MakeFailure(FString::Printf(
				TEXT("Output pin '%s' not found on source node. Available: %s"),
				*SourcePinName, *ListPinNames(SourceNode, EGPD_Output)));
		}

		UEdGraphPin* TargetPin = FindPin(TargetNode, TargetPinName, EGPD_Input);
		if (TargetPin == nullptr)
		{
			return MakeFailure(FString::Printf(
				TEXT("Input pin '%s' not found on target node. Available: %s"),
				*TargetPinName, *ListPinNames(TargetNode, EGPD_Input)));
		}

		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(SourceGraph->GetSchema());
		if (Schema == nullptr)
		{
			return MakeFailure(TEXT("Graph is not a Kismet (K2) graph"));
		}

		// Ask the schema first so we can report *why* a wiring is illegal instead
		// of just failing; TryCreateConnection alone gives no reason.
		const FPinConnectionResponse Response =
			Schema->CanCreateConnection(SourcePin, TargetPin);
		if (Response.Response == CONNECT_RESPONSE_DISALLOW)
		{
			return MakeFailure(FString::Printf(TEXT("Connection refused: %s"),
				*Response.Message.ToString()));
		}

		if (!Schema->TryCreateConnection(SourcePin, TargetPin))
		{
			return MakeFailure(TEXT("TryCreateConnection failed"));
		}

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("source"), FString::Printf(TEXT("%s.%s"), *SourceId, *SourcePinName));
		Data->SetStringField(TEXT("target"), FString::Printf(TEXT("%s.%s"), *TargetId, *TargetPinName));
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandRemoveNode(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		const FString NodeId = GetStringParam(Params, TEXT("node_id"));
		if (NodeId.IsEmpty())
		{
			return MakeFailure(TEXT("Missing required parameter: node_id"));
		}

		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = FindNodeByGuid(Blueprint, NodeId, Graph);
		if (Node == nullptr)
		{
			return MakeFailure(FString::Printf(TEXT("Node not found: %s"), *NodeId));
		}
		if (!Node->CanUserDeleteNode())
		{
			return MakeFailure(FString::Printf(
				TEXT("Node '%s' cannot be deleted"),
				*Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));
		}

		const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);

		CompileAndSave(Blueprint);

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("removed"), NodeId);
		Data->SetStringField(TEXT("title"), Title);
		return MakeSuccess(Data);
	}

	TSharedRef<FJsonObject> CommandListNodes(const TSharedPtr<FJsonObject>& Params)
	{
		FString Error;
		UBlueprint* Blueprint = LoadBlueprintParam(Params, Error);
		if (Blueprint == nullptr)
		{
			return MakeFailure(Error);
		}

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);

		TArray<TSharedPtr<FJsonValue>> GraphEntries;
		int32 TotalNodes = 0;

		for (UEdGraph* Graph : Graphs)
		{
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr)
				{
					Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(Node)));
					++TotalNodes;
				}
			}

			const TSharedRef<FJsonObject> GraphJson = MakeShared<FJsonObject>();
			GraphJson->SetStringField(TEXT("name"), Graph->GetName());
			GraphJson->SetArrayField(TEXT("nodes"), Nodes);
			GraphEntries.Add(MakeShared<FJsonValueObject>(GraphJson));
		}

		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), true);
		Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Data->SetNumberField(TEXT("node_count"), TotalNodes);
		Data->SetArrayField(TEXT("graphs"), GraphEntries);
		return MakeSuccess(Data);
	}
}

namespace MCPBridgeCommands
{
	TSharedRef<FJsonObject> Dispatch(const FString& Command, const TSharedPtr<FJsonObject>& Params)
	{
		check(IsInGameThread());

		UE_LOG(LogMCPBridge, Verbose, TEXT("Dispatching '%s'"), *Command);

		if (Command == TEXT("get_capabilities")) { return CommandGetCapabilities(); }
		if (Command == TEXT("create_blueprint")) { return CommandCreateBlueprint(Params); }
		if (Command == TEXT("add_component"))    { return CommandAddComponent(Params); }
		if (Command == TEXT("add_variable"))     { return CommandAddVariable(Params); }
		if (Command == TEXT("add_function"))     { return CommandAddFunction(Params); }
		if (Command == TEXT("add_node"))         { return CommandAddNode(Params); }
		if (Command == TEXT("connect_nodes"))    { return CommandConnectNodes(Params); }
		if (Command == TEXT("remove_node"))      { return CommandRemoveNode(Params); }
		if (Command == TEXT("list_nodes"))       { return CommandListNodes(Params); }

		return MakeFailure(FString::Printf(TEXT("Unknown command: %s"), *Command));
	}
}

import type { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import type { ConnectionManager } from "../transports/connection-manager.js";
import type { PluginBridgeResponse, UnrealMcpConfig } from "../types.js";
import { inlineScript } from "../utils/template.js";

/**
 * Render a plugin bridge response as a tool result.
 *
 * The plugin omits `data` when a command fails, so stringifying it
 * unconditionally yields `undefined` rather than a string, and the whole tool
 * result then fails MCP schema validation. The caller sees ~80 lines of Zod
 * complaints instead of the reason the command failed — which the plugin does
 * report, e.g. "Output pin 'nope' not found on source node. Available: ...".
 */
function pluginResult(response: PluginBridgeResponse, indent = false) {
	if (!response.success) {
		return {
			content: [
				{
					type: "text" as const,
					text: JSON.stringify({ error: response.error ?? "Plugin command failed" }),
				},
			],
		};
	}

	return {
		content: [
			{
				type: "text" as const,
				text: JSON.stringify(response.data ?? {}, null, indent ? 2 : undefined),
			},
		],
	};
}

export function registerBlueprintTools(
	server: McpServer,
	manager: ConnectionManager,
	_config: UnrealMcpConfig,
): void {
	server.tool(
		"create_blueprint",
		"Create a new Blueprint class.",
		{
			name: z.string().describe("Blueprint asset name"),
			parent_class: z
				.string()
				.default("Actor")
				.describe("Parent class (e.g., Actor, Character, Pawn, PlayerController)"),
			path: z.string().default("/Game/Blueprints").describe("Content directory to create in"),
		},
		async ({ name, parent_class, path }) => {
			manager.requireEditor();

			// Try plugin bridge first for richer creation
			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "create_blueprint",
					params: { name, parent_class, path },
				});
				return pluginResult(response);
			}

			// Python fallback
			const script = inlineScript(
				`import unreal
import json
factory = unreal.BlueprintFactory()
parent = getattr(unreal, '{{parent_class}}', None) or unreal.EditorAssetLibrary.load_asset('/Script/Engine.{{parent_class}}')
factory.set_editor_property('ParentClass', parent)
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
bp = asset_tools.create_asset('{{name}}', '{{path}}', unreal.Blueprint, factory)
if bp:
    print(json.dumps({"success": True, "name": bp.get_name(), "path": bp.get_path_name()}))
else:
    print(json.dumps({"error": "Failed to create Blueprint"}))`,
				{ name, parent_class, path },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"add_blueprint_component",
		"Add a component to a Blueprint.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			component_class: z
				.string()
				.describe(
					"Component class (e.g., StaticMeshComponent, BoxCollisionComponent, PointLightComponent)",
				),
			component_name: z.string().optional().describe("Name for the component"),
		},
		async ({ blueprint_path, component_class, component_name }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "add_component",
					params: { blueprint_path, component_class, component_name },
				});
				return pluginResult(response);
			}

			const cname = component_name || component_class.replace("Component", "");
			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    comp_class = getattr(unreal, '{{component_class}}', None)
    if comp_class:
        try:
            subsys = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
            handles = subsys.k2_gather_subobject_data_for_blueprint(bp)
            root_handle = handles[0] if handles else None
            new_handle, fail = subsys.k2_add_new_subobject(unreal.AddNewSubobjectParams(parent_handle=root_handle, new_class=comp_class, blueprint_context=bp))
            if new_handle.is_valid():
                unreal.BlueprintEditorLibrary.compile_blueprint(bp)
                unreal.EditorAssetLibrary.save_asset('{{blueprint_path}}')
                print(json.dumps({"success": True, "component": "{{cname}}"}))
            else:
                print(json.dumps({"error": "Failed to add component: " + str(fail)}))
        except Exception as e:
            print(json.dumps({"error": str(e)}))
    else:
        print(json.dumps({"error": "Component class not found: {{component_class}}"}))
else:
    print(json.dumps({"error": "Blueprint not found: {{blueprint_path}}"}))`,
				{ blueprint_path, component_class, cname, component_name: cname },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"add_blueprint_variable",
		"Add a variable to a Blueprint.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			variable_name: z.string().describe("Variable name"),
			variable_type: z
				.enum(["bool", "int", "float", "string", "vector", "rotator", "object"])
				.describe("Variable type"),
			default_value: z.string().optional().describe("Default value as string"),
		},
		async ({ blueprint_path, variable_name, variable_type, default_value }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "add_variable",
					params: { blueprint_path, variable_name, variable_type, default_value },
				});
				return pluginResult(response);
			}

			const typeMap: Record<string, string> = {
				bool: "bool",
				int: "int",
				float: "float",
				string: "string",
				vector: "struct'/Script/CoreUObject.Vector'",
				rotator: "struct'/Script/CoreUObject.Rotator'",
				object: "object'/Script/Engine.Object'",
			};
			const pinType = typeMap[variable_type] || "bool";

			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    unreal.BlueprintEditorLibrary.add_member_variable(bp, '{{variable_name}}', '${pinType}')
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    print(json.dumps({"success": True, "variable": "{{variable_name}}", "type": "${variable_type}"}))
else:
    print(json.dumps({"error": "Blueprint not found"}))`,
				{ blueprint_path, variable_name },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"add_graph_node",
		"Add a node to a Blueprint's event graph. Requires the optional C++ plugin for full node type support. Falls back to Python for basic operations.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			node_type: z
				.string()
				.describe(
					"Node type (e.g., Branch, Print, CallFunction, VariableGet, VariableSet, ReceiveBeginPlay, ReceiveTick, Comparison, Switch, ExecutionSequence, SpawnActor, DynamicCast, etc.)",
				),
			x: z.number().default(0).describe("X position in graph"),
			y: z.number().default(0).describe("Y position in graph"),
			properties: z
				.record(z.unknown())
				.optional()
				.describe(
					"Node-specific properties (e.g., {function_name: 'PrintString', target_class: 'KismetSystemLibrary'})",
				),
		},
		async ({ blueprint_path, node_type, x, y, properties }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "add_node",
					params: { blueprint_path, node_type, x, y, properties: properties || {} },
				});
				return pluginResult(response);
			}

			// Python fallback — limited to basic node types
			return {
				content: [
					{
						type: "text",
						text: JSON.stringify({
							error:
								"Full node graph editing requires the UnrealMCPBridge plugin. Without it, use create_blueprint, add_blueprint_component, add_blueprint_variable, and compile_blueprint for basic Blueprint operations.",
							hint: "Install the plugin from plugin/UnrealMCPBridge/ in your UE project's Plugins directory.",
						}),
					},
				],
			};
		},
	);

	server.tool(
		"connect_graph_nodes",
		"Wire two node pins together in a Blueprint graph. Requires the optional C++ plugin.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			source_node_id: z.string().describe("Source node ID"),
			source_pin: z.string().describe("Source pin name"),
			target_node_id: z.string().describe("Target node ID"),
			target_pin: z.string().describe("Target pin name"),
		},
		async ({ blueprint_path, source_node_id, source_pin, target_node_id, target_pin }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "connect_nodes",
					params: { blueprint_path, source_node_id, source_pin, target_node_id, target_pin },
				});
				return pluginResult(response);
			}

			return {
				content: [
					{
						type: "text",
						text: JSON.stringify({
							error: "Node pin wiring requires the UnrealMCPBridge plugin.",
							hint: "Install the plugin from plugin/UnrealMCPBridge/ in your UE project's Plugins directory.",
						}),
					},
				],
			};
		},
	);

	server.tool(
		"remove_graph_node",
		"Remove a node from a Blueprint graph. Requires the optional C++ plugin.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			node_id: z.string().describe("Node ID to remove"),
		},
		async ({ blueprint_path, node_id }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "remove_node",
					params: { blueprint_path, node_id },
				});
				return pluginResult(response);
			}

			return {
				content: [
					{
						type: "text",
						text: JSON.stringify({ error: "Node removal requires the UnrealMCPBridge plugin." }),
					},
				],
			};
		},
	);

	server.tool(
		"list_graph_nodes",
		"List all nodes in a Blueprint's event graph.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
		},
		async ({ blueprint_path }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "list_nodes",
					params: { blueprint_path },
				});
				return pluginResult(response, true);
			}

			// Python fallback — graph node enumeration is not available via Python API
			// (ubergraph_pages is not exposed). Return helpful message.
			return {
				content: [
					{
						type: "text",
						text: JSON.stringify({
							error:
								"Blueprint graph node listing requires the UnrealMCPBridge plugin. The Blueprint graph API (ubergraph_pages) is not exposed to Python.",
							hint: "Install the plugin from plugin/UnrealMCPBridge/ in your UE project's Plugins directory.",
						}),
					},
				],
			};
		},
	);

	server.tool(
		"compile_blueprint",
		"Compile a Blueprint.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
		},
		async ({ blueprint_path }) => {
			manager.requireEditor();
			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    unreal.EditorAssetLibrary.save_asset('{{blueprint_path}}')
    print(json.dumps({"success": True, "compiled": "{{blueprint_path}}"}))
else:
    print(json.dumps({"error": "Blueprint not found: {{blueprint_path}}"}))`,
				{ blueprint_path },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"get_blueprint_info",
		"Get Blueprint info: class, variables, functions, components, and graphs.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
		},
		async ({ blueprint_path }) => {
			manager.requireEditor();
			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    result = {"name": bp.get_name(), "path": bp.get_path_name()}
    try:
        gen = bp.generated_class()
        if gen:
            super_class = gen.get_super_class()
            if super_class:
                result["parent_class"] = super_class.get_name()
    except:
        pass
    try:
        gen_class = bp.generated_class()
        if gen_class:
            result["generated_class"] = gen_class.get_name()
    except:
        pass
    try:
        gen = bp.generated_class()
        if gen:
            # Spawn temp actor to get all components including inherited
            subsys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
            temp = subsys.spawn_actor_from_class(gen, unreal.Vector(0, 0, -100000))
            if temp:
                all_comps = temp.get_components_by_class(unreal.ActorComponent)
                result["components"] = [{"name": c.get_name(), "class": c.get_class().get_name()} for c in all_comps]
                subsys.destroy_actor(temp)
    except:
        pass
    print(json.dumps(result, indent=2))
else:
    print(json.dumps({"error": "Blueprint not found: {{blueprint_path}}"}))`,
				{ blueprint_path },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"spawn_blueprint_actor",
		"Spawn an instance of a Blueprint class in the level.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			label: z.string().optional().describe("Actor label"),
			location: z
				.object({ x: z.number(), y: z.number(), z: z.number() })
				.default({ x: 0, y: 0, z: 0 }),
			rotation: z
				.object({ pitch: z.number(), yaw: z.number(), roll: z.number() })
				.default({ pitch: 0, yaw: 0, roll: 0 }),
		},
		async ({ blueprint_path, label, location, rotation }) => {
			manager.requireEditor();
			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    loc = unreal.Vector({{loc_x}}, {{loc_y}}, {{loc_z}})
    rot = unreal.Rotator({{rot_pitch}}, {{rot_yaw}}, {{rot_roll}})
    subsys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsys.spawn_actor_from_class(bp.generated_class(), loc, rot)
    if actor:
        label = '{{label}}'
        if label:
            actor.set_actor_label(label)
        print(json.dumps({"success": True, "name": actor.get_name()}))
    else:
        print(json.dumps({"error": "Failed to spawn Blueprint actor"}))
else:
    print(json.dumps({"error": "Blueprint not found: {{blueprint_path}}"}))`,
				{
					blueprint_path,
					label: label || "",
					loc_x: location.x,
					loc_y: location.y,
					loc_z: location.z,
					rot_pitch: rotation.pitch,
					rot_yaw: rotation.yaw,
					rot_roll: rotation.roll,
				},
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"add_blueprint_function",
		"Add a custom function to a Blueprint.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			function_name: z.string().describe("Function name"),
		},
		async ({ blueprint_path, function_name }) => {
			manager.requireEditor();

			if (manager.hasPlugin) {
				const response = await manager.plugin.sendCommand({
					command: "add_function",
					params: { blueprint_path, function_name },
				});
				return pluginResult(response);
			}

			const script = inlineScript(
				`import unreal
import json
bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if bp:
    unreal.BlueprintEditorLibrary.add_function_graph(bp, '{{function_name}}')
    unreal.EditorAssetLibrary.save_asset('{{blueprint_path}}')
    print(json.dumps({"success": True, "function": "{{function_name}}"}))
else:
    print(json.dumps({"error": "Blueprint not found"}))`,
				{ blueprint_path, function_name },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);

	server.tool(
		"set_blueprint_property",
		"Set a default property value on a Blueprint's CDO (Class Default Object). For component properties, use a dot path like 'ComponentName.PropertyName'.",
		{
			blueprint_path: z.string().describe("Blueprint asset path"),
			property_name: z
				.string()
				.describe("Property name, or ComponentName.PropertyName for component sub-properties"),
			property_value: z
				.string()
				.describe(
					"Property value — asset paths (/Game/...), Python expressions (True, 42.0), or plain strings",
				),
		},
		async ({ blueprint_path, property_name, property_value }) => {
			manager.requireEditor();
			const script = inlineScript(
				`import unreal
import json

bp = unreal.EditorAssetLibrary.load_asset('{{blueprint_path}}')
if not bp:
    print(json.dumps({"error": "Blueprint not found: {{blueprint_path}}"}))
else:
    prop_name = '{{property_name}}'
    raw_value = '{{property_value}}'

    # Auto-load asset paths
    value = raw_value
    if raw_value.startswith('/Game/') or raw_value.startswith('/Script/') or raw_value.startswith('/Engine/'):
        loaded = unreal.EditorAssetLibrary.load_asset(raw_value)
        if loaded:
            value = loaded
        else:
            print(json.dumps({"error": "Could not load asset: " + raw_value}))
            raise SystemExit()
    elif raw_value in ('True', 'False'):
        value = raw_value == 'True'
    elif raw_value == 'None':
        value = None
    else:
        try:
            value = float(raw_value) if '.' in raw_value else int(raw_value)
        except ValueError:
            value = raw_value

    # Handle dot-path for component properties (e.g., BodyMesh.StaticMesh)
    if '.' in prop_name:
        comp_name, sub_prop = prop_name.split('.', 1)
        # Spawn a temporary actor to access all components (including inherited)
        gen = bp.generated_class()
        if not gen:
            print(json.dumps({"error": "Could not get generated class"}))
        else:
            # Try CDO first
            cdo = unreal.get_default_object(gen)
            target_comp = None
            if cdo:
                for comp in cdo.get_components_by_class(unreal.ActorComponent):
                    if comp.get_name() == comp_name or comp_name in comp.get_name():
                        target_comp = comp
                        break
            # If not found in CDO, spawn a temp actor to find inherited components
            if not target_comp:
                subsys = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
                temp = subsys.spawn_actor_from_class(gen, unreal.Vector(0, 0, -100000))
                if temp:
                    for comp in temp.get_components_by_class(unreal.ActorComponent):
                        if comp.get_name() == comp_name or comp_name in comp.get_name():
                            # Found it on the instance — now set on CDO template
                            # Find matching component on CDO by class
                            if cdo:
                                for cdo_comp in cdo.get_components_by_class(comp.get_class()):
                                    target_comp = cdo_comp
                                    break
                            break
                    subsys.destroy_actor(temp)
            if target_comp:
                try:
                    if sub_prop == 'StaticMesh' and hasattr(target_comp, 'set_static_mesh'):
                        target_comp.set_static_mesh(value)
                    elif sub_prop == 'SkeletalMesh' and hasattr(target_comp, 'set_skeletal_mesh_asset'):
                        target_comp.set_skeletal_mesh_asset(value)
                    else:
                        target_comp.set_editor_property(sub_prop, value)
                    unreal.EditorAssetLibrary.save_asset('{{blueprint_path}}')
                    print(json.dumps({"success": True, "component": comp_name, "property": sub_prop}))
                except Exception as e:
                    print(json.dumps({"error": str(e)}))
            else:
                # List available components to help the user
                available = []
                if cdo:
                    available = [c.get_name() for c in cdo.get_components_by_class(unreal.ActorComponent)]
                print(json.dumps({"error": "Component not found: " + comp_name, "available_components": available, "hint": "Component may be inherited. Try using execute_python for direct access."}))
    else:
        # Top-level CDO property
        try:
            gen_class = bp.generated_class()
            cdo = unreal.get_default_object(gen_class) if gen_class else None
            if cdo:
                cdo.set_editor_property(prop_name, value)
                print(json.dumps({"success": True}))
            else:
                print(json.dumps({"error": "Could not get CDO"}))
        except Exception as e:
            print(json.dumps({"error": str(e)}))`,
				{ blueprint_path, property_name, property_value },
			);
			const result = await manager.runPython(script);
			return { content: [{ type: "text", text: result }] };
		},
	);
}

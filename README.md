# unreal-mcp

The most comprehensive MCP server for Unreal Engine — **127 tools** across **16 subsystems**, with **4 transport layers** and **no mandatory C++ plugin**.

> **Beta** — This project is under active development and testing. Tools are being validated against UE 5.6. Some tools may not work as expected. Bug reports and contributions are welcome.

## Why This One?

| | unreal-mcp | [flopperam](https://github.com/flopperam/unreal-engine-mcp) | [chongdashu](https://github.com/chongdashu/unreal-mcp) | [kvick-games](https://github.com/kvick-games/UnrealMCP) | [ChiR24](https://github.com/ChiR24/Unreal_mcp) |
|---|---|---|---|---|---|
| Tools | **127** | ~30 | ~20 | ~5 | 36 |
| Transports | **4** | 1 | 1 | 1 | 1 |
| Requires C++ plugin | **No** | Yes | Yes | Yes | Yes |
| Build/package tools | **Yes** | No | No | No | Partial |

Most Unreal MCP projects require compiling and installing a custom C++ plugin into your UE project. This one works out of the box by using Unreal's built-in Python and Remote Control plugins — zero-install beyond enabling what already ships with UE.

## Quick Start

### Prerequisites

- Node.js >= 18
- Unreal Engine 5.x with editor open
- **Python Editor Script Plugin** enabled (built-in) with **Enable Remote Execution** checked in its settings

### Install

```bash
git clone https://github.com/YOUR_USERNAME/unreal-mcp.git
cd unreal-mcp
npm install
npm run build
```

### Add to Claude Code

**Per-project** (from your UE project directory):
```bash
claude mcp add --transport stdio unreal-mcp -- node /path/to/unreal-mcp/dist/bin.js
```

**Global** (available in all projects):
```bash
claude mcp add --scope user --transport stdio unreal-mcp -- node /path/to/unreal-mcp/dist/bin.js
```

Then drop a `.unrealmcp.json` in each UE project:
```json
{
  "projectPath": "."
}
```

### Add to Claude Desktop

Add to `%APPDATA%\Claude\claude_desktop_config.json` (Windows) or `~/Library/Application Support/Claude/claude_desktop_config.json` (macOS):

```json
{
  "mcpServers": {
    "unreal": {
      "command": "node",
      "args": ["/path/to/unreal-mcp/dist/bin.js"],
      "env": {
        "UNREAL_MCP_PROJECT_PATH": "/path/to/YourProject.uproject"
      }
    }
  }
}
```

## Tool Modules

| Module | Tools | Description |
|--------|-------|-------------|
| **actor** | 10 | Spawn, delete, transform, select, duplicate, tag actors |
| **asset** | 16 | List, search, import, export, rename, delete, validate assets |
| **blueprint** | 12 | Create blueprints, add components/variables/functions, graph nodes |
| **build** | 9 | Build targets, cook content, package, generate project files |
| **material** | 13 | Create materials/instances, add expressions, wire graphs |
| **console** | 6 | Execute Python, console commands, screenshots, viewport camera |
| **sequencer** | 8 | Create sequences, add tracks/bindings, set playback range |
| **animation** | 6 | Animation blueprints, montages, modifiers, skeletal mesh |
| **niagara** | 8 | Spawn particle systems, set parameters (float/vector/color/bool) |
| **editor-utils** | 8 | Undo/redo, LOD generation, collision, lightmap UVs, utility widgets |
| **testing** | 8 | Automation tests, map check, data validation, Gauntlet |
| **profiling** | 5 | CSV profiling, Unreal Insights traces, stat commands |
| **source-control** | 6 | Status, checkout, checkin, revert, mark for add, diff |
| **world-partition** | 4 | Data layers, streaming sources, loaded cells |
| **remote-control-presets** | 5 | List/get/set preset properties, call preset functions |
| **plugin** | 3 | List, enable, disable plugins in .uproject |

## Architecture

```
MCP Client (Claude Code, Claude Desktop, etc.)
  ↕ stdio (MCP protocol)
unreal-mcp server
  ↕ 4 transport layers
Unreal Engine
```

### Transport Layers

| Transport | Protocol | Port | What It Needs |
|-----------|----------|------|---------------|
| **Python Remote Execution** | UDP multicast + inverted TCP | 6776 | Python Editor Script Plugin (built-in) |
| **Remote Control API** | HTTP REST | 30010 | Remote Control API plugin (built-in) |
| **Plugin Bridge** | TCP, length-prefixed JSON | 55557 | Optional C++ plugin |
| **Subprocess Runner** | Spawns UAT/UBT processes | N/A | Engine path only |

The server probes all transports on startup and tools gracefully degrade. Most tools use Python Remote Execution. Build tools use subprocess. The optional C++ plugin adds deep Blueprint graph manipulation.

### Two Paths

- **Core path** (no plugin): Python + Remote Control covers ~95% of tools. Just enable the built-in UE plugins.
- **Plugin path** (optional): C++ plugin on port 55557 adds K2 node graph manipulation, faster bulk operations, and editor UI integration. Falls back to Python automatically when unavailable.

## Configuration

Three-layer priority: CLI args > environment variables > config file > defaults.

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `UNREAL_MCP_PROJECT_PATH` | — | Path to .uproject file or project directory |
| `UNREAL_MCP_ENGINE_PATH` | auto-detect | UE engine install path |
| `UNREAL_MCP_RC_PORT` | 30010 | Remote Control API port |
| `UNREAL_MCP_PYTHON_PORT` | 6776 | Python Remote Execution port |
| `UNREAL_MCP_PLATFORM` | Win64 | Target platform |
| `UNREAL_MCP_CONFIGURATION` | Development | Build configuration |
| `UNREAL_MCP_MODULES` | all | Comma-separated list of modules to enable |

### CLI Arguments

```bash
node dist/bin.js --project-path /path/to/project --engine-path /path/to/UE_5.5 --rc-port 30010
```

### Config File

Place `.unrealmcp.json` in your project directory or home directory:

```json
{
  "projectPath": ".",
  "platform": "Win64",
  "configuration": "Development",
  "enabledModules": ["console", "actor", "asset", "build", "blueprint", "material"]
}
```

## Unreal Editor Setup

### Required (for most tools)

1. Edit > Plugins > enable **Python Editor Script Plugin**
2. Restart the editor
3. Edit > Project Settings > Plugins > **Python** > scroll to **Remote Execution** section:
   - Check **Enable Remote Execution**
   - **UE 5.3+ IMPORTANT:** Change **Multicast Bind Address** from `127.0.0.1` to `0.0.0.0` — Epic changed the default in 5.3 and it breaks external tools
   - Verify Multicast Group Endpoint is `239.0.0.1:6766`
4. Restart the editor again

**Still getting "No Unreal Editor nodes found"?**
- **VPN/Tailscale users:** Tailscale's virtual network adapter can hijack multicast. Try temporarily disabling Tailscale, or disable the Tailscale network adapter in Windows Network Connections.
- **Firewall:** Allow UDP port 6766 and TCP port 6776, or temporarily disable Windows Firewall to test.
- **Multiple adapters:** WSL, Hyper-V, and VPN adapters can all cause multicast to bind to the wrong interface. Disabling unused adapters helps.

### Optional (for Remote Control tools)

1. Edit > Plugins > enable **Remote Control API**
2. Restart the editor
3. Edit > Project Settings > Plugins > **Remote Control** > **Server**:
   - Check **Restrict Server Access** — this sounds restrictive but actually *enables* the sub-options below (unchecked = features hidden/off)
   - Check **Enable Remote Python Execution**
   - Check **Allow Console Command Remote Execution**
   - Allowed Origins: leave blank or add `127.0.0.1`
   - These take effect immediately, no restart needed

### Optional (for Blueprint graph tools)

Copy `plugin/UnrealMCPBridge/` into your project's `Plugins/` directory, add it to the
`Plugins` array in your `.uproject`, then rebuild the editor target:

```
"<UE>/Engine/Build/BatchFiles/Build.bat" <Project>Editor Win64 Development -Project="<path>.uproject"
```

The editor must be closed while building. Requires a C++ project — a Blueprint-only
project has no editor target to compile the plugin into.

This is what enables `add_graph_node`, `connect_graph_nodes`, `remove_graph_node` and
`list_graph_nodes`, none of which have a Python fallback: the Blueprint graph API
(`ubergraph_pages`) is not exposed to Python. The plugin also serves `create_blueprint`,
`add_blueprint_component`, `add_blueprint_variable` and `add_blueprint_function`, which
otherwise fall back to Python.

The bridge listens on `127.0.0.1:55557`. Pass `-MCPBridgePort=<n>` on the editor command
line if you run two editors at once, so the second does not lose the bind race.

## Development

```bash
npm run dev        # Watch-mode dev server
npm run build      # Compile TypeScript
npm run lint       # Biome linter
npm run fmt        # Biome formatter
npm test           # Run tests
```

## License

MIT

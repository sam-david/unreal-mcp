import { randomUUID } from "node:crypto";
import { type Socket, createConnection } from "node:net";
import type { PluginBridgeCommand, PluginBridgeResponse, PluginCapabilities } from "../types.js";
import { PluginNotAvailableError, TimeoutError, UnrealMcpError } from "../utils/errors.js";

export interface PluginBridgeConfig {
	host: string;
	port: number;
	timeout: number;
}

/**
 * TCP client for the optional UnrealMCPBridge C++ plugin.
 * Default port 55557. Provides deep K2Node/Blueprint graph manipulation
 * that isn't available through Python or Remote Control APIs.
 *
 * Protocol: JSON commands over TCP, same pattern as flopperam/chongdashu.
 * Messages are length-prefixed (4-byte LE uint32).
 *
 * Supports:
 * - Persistent connection with auto-reconnect
 * - Capability negotiation (get_capabilities command)
 * - Request IDs for future multiplexing
 * - Proper streaming length-prefix frame parsing
 */
export class PluginBridgeClient {
	private host: string;
	private port: number;
	private timeout: number;
	private _available = false;
	private _capabilities: PluginCapabilities | null = null;
	private socket: Socket | null = null;
	private receiveBuffer = Buffer.alloc(0);
	private pendingRequests = new Map<
		string,
		{
			resolve: (response: PluginBridgeResponse) => void;
			reject: (error: Error) => void;
			timer: ReturnType<typeof setTimeout>;
		}
	>();

	constructor(config: PluginBridgeConfig) {
		this.host = config.host;
		this.port = config.port;
		this.timeout = config.timeout;
	}

	get available(): boolean {
		return this._available;
	}

	get capabilities(): PluginCapabilities | null {
		return this._capabilities;
	}

	/**
	 * Check if the plugin supports a specific command.
	 */
	hasCapability(command: string): boolean {
		if (!this._capabilities) return false;
		return this._capabilities.commands.includes(command);
	}

	/**
	 * Check if the plugin supports a specific feature flag.
	 */
	hasFeature(feature: string): boolean {
		if (!this._capabilities) return false;
		return this._capabilities.features.includes(feature);
	}

	async isAvailable(): Promise<boolean> {
		try {
			// Try to establish connection and negotiate capabilities
			await this.ensureConnected();
			this._available = true;
			return true;
		} catch {
			this._available = false;
			this._capabilities = null;
			return false;
		}
	}

	private async ensureConnected(): Promise<void> {
		if (this.socket && !this.socket.destroyed) {
			return;
		}

		await new Promise<void>((resolve, reject) => {
			let connected = false;
			const socket = createConnection({ host: this.host, port: this.port }, () => {
				connected = true;
				// The 3s budget below is meant to bound *connecting*. Node's
				// setTimeout is an idle timer that stays armed for the life of the
				// socket, so leaving it set would tear down a perfectly healthy
				// connection 3s after the last message and flip _available to false.
				socket.setTimeout(0);
				this.socket = socket;
				this.receiveBuffer = Buffer.alloc(0);
				resolve();
			});
			socket.setTimeout(3000);
			socket.on("timeout", () => {
				if (connected) {
					return;
				}
				socket.destroy();
				reject(new Error("Connection timeout"));
			});
			socket.on("error", (err) => {
				socket.destroy();
				reject(err);
			});

			// Set up persistent connection handlers
			socket.on("data", (data) => this.handleData(data));
			socket.on("end", () => this.handleDisconnect());
			socket.on("close", () => this.handleDisconnect());
		});

		// Negotiate capabilities after connecting
		await this.negotiateCapabilities();
	}

	private handleData(data: Buffer): void {
		this.receiveBuffer = Buffer.concat([this.receiveBuffer, data]);
		this.processBuffer();
	}

	/**
	 * Process accumulated receive buffer, extracting complete length-prefixed messages.
	 */
	private processBuffer(): void {
		while (this.receiveBuffer.length >= 4) {
			const msgLen = this.receiveBuffer.readUInt32LE(0);

			// Sanity check: if msgLen looks unreasonable, try parsing as raw JSON
			if (msgLen > 10_000_000 || msgLen > this.receiveBuffer.length * 2) {
				this.tryParseRawJson();
				return;
			}

			if (this.receiveBuffer.length < 4 + msgLen) {
				break; // Incomplete message, wait for more data
			}

			const msgBytes = this.receiveBuffer.subarray(4, 4 + msgLen);
			this.receiveBuffer = this.receiveBuffer.subarray(4 + msgLen);

			try {
				const response = JSON.parse(msgBytes.toString("utf-8")) as PluginBridgeResponse;
				this.dispatchResponse(response);
			} catch {
				// If parsing fails, reject the oldest pending request
				const first = this.pendingRequests.values().next().value;
				if (first) {
					first.reject(new UnrealMcpError("Failed to parse plugin response", "PLUGIN_PARSE_ERROR"));
				}
			}
		}
	}

	/**
	 * Fallback: try to parse the entire buffer as raw JSON (no length prefix).
	 * This handles plugins that don't use length-prefixed framing.
	 */
	private tryParseRawJson(): void {
		try {
			const response = JSON.parse(this.receiveBuffer.toString("utf-8")) as PluginBridgeResponse;
			this.receiveBuffer = Buffer.alloc(0);
			this.dispatchResponse(response);
		} catch {
			// Not valid JSON yet, wait for more data
		}
	}

	private dispatchResponse(response: PluginBridgeResponse): void {
		// Match by response ID if present
		if (response.id && this.pendingRequests.has(response.id)) {
			const pending = this.pendingRequests.get(response.id) as NonNullable<
				ReturnType<typeof this.pendingRequests.get>
			>;
			this.pendingRequests.delete(response.id);
			clearTimeout(pending.timer);
			pending.resolve(response);
			return;
		}

		// No ID match — resolve the oldest pending request (FIFO)
		const firstKey = this.pendingRequests.keys().next().value;
		if (firstKey) {
			const pending = this.pendingRequests.get(firstKey) as NonNullable<
				ReturnType<typeof this.pendingRequests.get>
			>;
			this.pendingRequests.delete(firstKey);
			clearTimeout(pending.timer);
			pending.resolve(response);
		}
	}

	private handleDisconnect(): void {
		this.socket = null;
		this._available = false;
		// Reject all pending requests
		for (const [id, pending] of this.pendingRequests) {
			clearTimeout(pending.timer);
			pending.reject(new UnrealMcpError("Plugin bridge disconnected", "PLUGIN_CONNECTION_FAILED"));
			this.pendingRequests.delete(id);
		}
	}

	/**
	 * Negotiate capabilities with the plugin. Sends get_capabilities and caches the result.
	 * If the plugin doesn't support get_capabilities, falls back to default capabilities.
	 */
	private async negotiateCapabilities(): Promise<void> {
		try {
			const response = await this.sendCommandInternal({
				id: randomUUID(),
				command: "get_capabilities",
				params: {},
			});
			if (response.success && response.data) {
				this._capabilities = response.data as PluginCapabilities;
			} else {
				// Plugin connected but doesn't support capability negotiation — assume basic set
				this._capabilities = {
					version: "unknown",
					commands: [
						"create_blueprint",
						"add_component",
						"add_variable",
						"add_node",
						"connect_nodes",
						"remove_node",
						"list_nodes",
						"add_function",
					],
					features: [],
				};
			}
		} catch {
			// Capability negotiation failed — assume basic set
			this._capabilities = {
				version: "unknown",
				commands: [
					"create_blueprint",
					"add_component",
					"add_variable",
					"add_node",
					"connect_nodes",
					"remove_node",
					"list_nodes",
					"add_function",
				],
				features: [],
			};
		}
	}

	/**
	 * Send a command to the C++ plugin and receive the response.
	 * Automatically reconnects on connection failure (one retry).
	 */
	async sendCommand(command: PluginBridgeCommand): Promise<PluginBridgeResponse> {
		// `_available` goes false whenever the socket drops — an editor restart, a
		// plugin reload — and nothing sets it back except isAvailable(), which only
		// runs on a status refresh. Without this retry the first call after the
		// editor comes back reports the plugin as absent even though it is
		// listening, and the caller is told to go install something already there.
		if (!this._available && !(await this.isAvailable())) {
			throw new PluginNotAvailableError();
		}

		// Assign request ID if not present
		const cmd = command.id ? command : { ...command, id: randomUUID() };

		try {
			await this.ensureConnected();
			return await this.sendCommandInternal(cmd);
		} catch (error) {
			// One retry on connection failure
			if (error instanceof UnrealMcpError && error.code === "PLUGIN_CONNECTION_FAILED") {
				try {
					this.socket?.destroy();
					this.socket = null;
					await this.ensureConnected();
					return await this.sendCommandInternal(cmd);
				} catch {
					this._available = false;
					throw error;
				}
			}
			throw error;
		}
	}

	/**
	 * Internal send: writes the length-prefixed command and waits for response.
	 */
	private sendCommandInternal(command: PluginBridgeCommand): Promise<PluginBridgeResponse> {
		return new Promise<PluginBridgeResponse>((resolve, reject) => {
			if (!this.socket || this.socket.destroyed) {
				reject(new UnrealMcpError("Plugin bridge not connected", "PLUGIN_CONNECTION_FAILED"));
				return;
			}

			const requestId = command.id || randomUUID();

			const timer = setTimeout(() => {
				this.pendingRequests.delete(requestId);
				reject(new TimeoutError(`Plugin command: ${command.command}`, this.timeout));
			}, this.timeout);

			this.pendingRequests.set(requestId, { resolve, reject, timer });

			const message = JSON.stringify(command);
			const messageBuffer = Buffer.from(message, "utf-8");
			const lengthBuffer = Buffer.alloc(4);
			lengthBuffer.writeUInt32LE(messageBuffer.length, 0);
			this.socket.write(Buffer.concat([lengthBuffer, messageBuffer]));
		});
	}

	async disconnect(): Promise<void> {
		if (this.socket && !this.socket.destroyed) {
			this.socket.destroy();
		}
		this.socket = null;
		this._available = false;
		this._capabilities = null;
		// Clean up pending requests
		for (const [id, pending] of this.pendingRequests) {
			clearTimeout(pending.timer);
			this.pendingRequests.delete(id);
		}
	}
}

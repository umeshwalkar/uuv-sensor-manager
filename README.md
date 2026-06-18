# Sensor Manager

A lightweight C++17 application for sensors manager.
It connects over TCP, UDP, or RS-232, sends startup configuration commands, reads measurement lines, parses them, and publishes JSON payloads to MQTT.

<p align="center">&star;&nbsp;&star;&nbsp;&star;&nbsp;</p>
<p align="center">If you like concept, please give it a &star; star</p>
<p align="center">or fork it and contribute!</p>
<p align="center">&star;&nbsp;&star;&nbsp;&star;&nbsp;</p>

---

## Quick setup after cloning

```bash
git clone https://github.com/<your-org>/uuv-sensor-manager.git
cd uuv-sensor-manager
```

### Open it in Visual Studio Code

1. Start [**Docker Desktop**](https://docs.docker.com/desktop/) or ensure the Docker daemon is running.
2. Open VS Code.
3. Install the [**Dev Containers**](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) extension if not installed.
4. Choose `File > Open Folder...` and select the cloned `uuv-sensor-manager` folder.
5. Open the Command Palette (`Ctrl+Shift+P`) and select **Dev Containers: Reopen in Container**.
6. Wait for the container to build and reopen the workspace inside it.

Once the container is open, you can build and run the project from the integrated terminal or using VS Code tasks.

---

## Build and Run

### Inside the dev container

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)
./build/sensor_manager config/sensor_config.json
```

### VS Code tasks

- `Terminal > Run Task... > build`
- `Terminal > Run Task... > run`

The `run` task launches `./build/sensor_manager` from the build folder and passes `config/sensor_config.json` as the argument.

### Debugging

To debug from VS Code, use a C++ debugger configuration with:

- `program`: `${workspaceFolder}/build/sensor_manager`
- `args`: `config/sensor_config.json`
- `cwd`: `${workspaceFolder}/build`

---

## Workspace layout

```
uuv-sensor-manager/
├── CMakeLists.txt
├── Dockerfile
├── README.md
├── config/
│   ├── sensor_config.json
│   └── sensor_config.ini
├── src/
│   ├── main.cpp
│   ├── Logger.hpp
│   ├── Logger.cpp
│   ├── Config.hpp
│   ├── Config.cpp
│   ├── Transport.hpp
│   ├── Transport.cpp
│   ├── MqttClient.hpp
│   ├── MqttClient.cpp
│   ├── SensorManager.hpp
│   ├── SensorManager.cpp
│   └── SensorParser.hpp
├── tests/
│   └── test_sensor.py
└── .vscode/
    └── tasks.json
```

---

## Configuration

The application supports two config formats:

- JSON: `config/sensor_config.json`
- INI: `config/sensor_config.ini`

The parser is selected from the file extension.

### Top-level config sections

- `debug` — logging settings
- `mqtt` — MQTT broker and topic settings
- `sensor` — transport, init commands, publish timing, and device mappings

### `debug`

| Field | Type | Meaning |
|-------|------|---------|
| `enabled` | bool | Enable debug/info logging (`true` enables INF/DBG output; ERR/WRN always print) |
| `level` | string | Log verbosity: `debug`, `info`, `warning`, `error` |

### `mqtt`

| Field | Type | Meaning |
|-------|------|---------|
| `enabled` | bool | Enable MQTT publish | 
| `broker` | string | MQTT broker hostname or IP |
| `port` | int | MQTT broker port |
| `client_id` | string | MQTT client identifier |
| `keepalive` | int | MQTT keepalive seconds |
| `qos` | int | MQTT QoS for publishes |
| `retain` | bool | MQTT retain flag |

#### `mqtt.topics.pub`

The JSON config supports a `mqtt.topics.pub` array of named publish topic definitions. The app uses the entries with `name`:

- `sensor`
- `status`

### `sensor`

The `sensor` section can contain both legacy flat fields and modern device/transport definitions.
The repo example uses:

- `sensor.transport[]` — transport definitions
- `sensor.devices[]` — device definitions referencing transports

#### `sensor.transport[]`

Each transport entry defines one physical or network path.

| Field | Type | Meaning |
|-------|------|---------|
| `id` | string | Unique transport id |
| `type` | string | `tcp_client`, `tcp_server`, `udp_server`, or `serial` |
| `bind_host` | string | Bind address for server transports |
| `bind_port` | int | Bind port for server transports |
| `host` | string | Remote host for TCP client |
| `port` | int | Remote port for TCP client |
| `serial_port` | string | Serial device path (e.g. `/dev/ttyUSB0`) |
| `serial_baud` | int | Serial baud rate |
| `serial_data_bits` | int | Serial data bits |
| `serial_stop_bits` | int | Serial stop bits |
| `serial_parity` | string | `N`, `E`, `O` |
| `connect_timeout_sec` | int | Connection timeout |
| `reconnect_delay_sec` | int | Reconnect delay |
| `read_timeout_ms` | int | Read timeout for `readLine()` |
| `buffer_size_bytes` | int | Receive buffer size |

#### `sensor.devices[]`

Each device entry selects a transport and controls publish behavior.

| Field | Type | Meaning |
|-------|------|---------|
| `id` | int | Numeric device id |
| `name` | string | Friendly name |
| `publish_enabled` | bool | Enable publishing for this device |
| `publish_raw_data` | bool | Publish raw text instead of parsed JSON |
| `publish_interval_ms` | int | How often to publish the latest snapshot |
| `validate_checksum` | bool | Reserved for future checksum validation |
| `send_init_on_reconnect` | bool | Re-send `init_commands` after reconnect |
| `init_commands` | array | Commands sent on connect/reconnect |

##### `input_channels.data_rx`

| Field | Type | Meaning |
|-------|------|---------|
| `enabled` | bool | Enable this input channel |
| `debug` | bool | Enable channel debug logging |
| `data_timeout_sec` | double | Warn if no valid data within this interval |
| `transport.id` | string | Link to a `sensor.transport[]` id |
| `transport.read_timeout_ms` | int | Override transport read timeout |

##### `output_channels.commands`

| Field | Type | Meaning |
|-------|------|---------|
| `enabled` | bool | Enable the commands output channel |
| `debug` | bool | Enable debug logging for commands |
| `transport.id` | string | Transport id for commands |

### `sensor.init_commands`

A list of commands the manager sends after the transport opens. Each command is suffixed with `\r\n` and the manager waits 150 ms between commands.

---

## SensorManager internals

`SensorManager` is the runtime engine that:

- creates the configured transport
- opens and reconnects automatically on failure
- sends the `init_commands` sequence after each connect
- reads one ASCII line at a time from the sensor
- parses lines into `SensorData`
- publishes snapshots and status to MQTT
- handles clean shutdown

It runs two threads:

1. `receiveLoop()` — open transport, read lines, parse data, update the current snapshot
2. `publishLoop()` — periodically publish data and status at configured intervals

If MQTT is disabled, the app still reads and parses sensor data but does not publish to a broker.

---

## SensorParser details

`SensorParser` auto-detects the packet type in each incoming line.

### Supported formats

- NMEA-style lines beginning with `$`
- whitespace-delimited numeric output lines

### What it parses

- NMEA lines: expects a sentence with `ADSVP` and parses the numeric fields after it
- whitespace lines: extracts the first three numeric tokens from the line

Parsed values are stored as:

- `param1` — first numeric parameter
- `param2` — second numeric parameter
- `paramn` — nth numeric parameter

The parser serializes data to JSON with `SensorParser::toJSON()`, and the manager injects a `ts` field containing the Unix epoch receive timestamp.

---

## Example config values

The default JSON config contains:

- `debug.enabled` = `true`
- `debug.level` = `debug`
- `mqtt.enabled` = `true`
- `mqtt.broker` = `host.docker.internal`
- `mqtt.port` = `1883`
- `mqtt.topics.pub` → `uuv/sensor` and `sensor/status`
- `sensor.transport[0].type` = `tcp_client`
- `sensor.transport[0].host` = `host.docker.internal`
- `sensor.transport[0].port` = `4006`
- `sensor.devices[0].init_commands` = startup commands such as `$020;0`, `$016;4`
- `sensor.devices[0].input_channels.data_rx.transport.id` = `ch1`
- `sensor.devices[0].input_channels.data_rx.read_timeout_ms` = `1000`

Use these values as the starting point for your own sensor and MQTT environment.

## Notes

- The config filename extension decides the loader: `.json` or `.ini`.
- The `run` task passes `config/sensor_config.json` to the built executable.
- Any `sensor.devices[0].input_channels.data_rx.transport.id` value must match one of the `sensor.transport[].id` entries.
- If you change transport settings, update the device input channel to reference the correct transport id.
- All **sensor** word replace with your actual sensosr name like INS, Pressure, Depth, etc..
- The nmea sentence is used for example parsing; actual parser shall be as per the sensor device interface protocol.

---

## Disclaimer

THIS SOFTWARE IS PROVIDED BY THE CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

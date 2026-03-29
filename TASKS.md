# Project aim

This project implements a multiplatform GUI based on DearIMGUI that provides an interface to the [MADS framework](https://github.com/pbosetti/mads).

It must present a two sides interface (resizable):

* on the left, a `Publish` pane, where the user can publish messages to the MADS network of agents
* on the right, a `Receive` pane, where the traffic is presented to the user for inspection

This is a mostly a tool for debugging the communication between agents

# Implementation details

* Only use portable libraries, DearIMGUI for graphical frontend
* As for Dear ImGUI backend, picke either SDL3 or GLFW: select the one that makes it simpler to gave a single self-contained project that has minimum or zero dependencies (use FetchContent)
* Use the MADS C++ library, assuming that it is installed in `$(mads -p)/lib/libMadsCore.[dylib|so]` on macOS and Linux, `$(mads -p)/bin/MadsCore.dll` and `$(mads -p)/lib/MadsCore.lib` on Windows with headers in `$(mads -p)/include`
* MADS Install location in CMake shall be an option with the default read from `$(mads -p)`

# GUI features

## Connection details

On the top window border we need MADS broker address with a field for IP/hostname (default `localhost`) and a field for port (default `9092`): those have to be used to synthesize the broker URI as `tcp://<host>:<port>`, which in turn is used to open the connection.

We also have two buttons to select client private key and broker public key. If not used, we use unencrypted connection. Those files are normal text files ending with `.pub` or `.key`.

The app subscribes to all topics.

Finally, we have buttons to connect/disconnect from the broker. Disconnect shall be automatically called upon exit.

Use the `agent.hpp` header and the `Mads::Agent` class (namely `Mads::start_agent`, `Mads::connect`, `Mads::disconnect`, `Mads::publish`, `Mads::receive`), for connecting and dealing with publish/subscribe.

Values entered in this section shall be saved/restored upon relaunch, i.e. connection details shall be persisted (using the `imgui.ini` file is fine, or if not possible a JSON file saved in the working directory).

## Left panel: `Publish`

On the top we have a field to select the topic, then a text box area for entering a JSON text, and a button to publish it.

The JSON editor must auto-indent and code-color the JSON, conditionally disable the publish button if the JSON text is empty or not valid. For the first version, the editor does not need to be too fancy: just use fixed-width font, use different colors for strings, numbers and bools, and indent each level with 2 spaces.

## Right panel: `Receive`

Here we must have an expandable list of received topics, dynamically updated as new messages arrive. For each topic, only the last message is stored. Each topic shall be presented as:

* a button to expand/collapse the topic content (default collapsed)
* the topic name
* the time elapsed in seconds with decimals up to ms since last message of the same topic was received; this value shall be refreshed once per UI frame

Once expanded, a topic shall present the last JSON received (read-only), scrollable, indented and code-colored. In a second version, we could want each level of the JSON to be collapsible.

On the bottom there is a button to clean the topics tree.


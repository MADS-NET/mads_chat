#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <portable-file-dialogs.h>

#include <agent.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kWindowTitle = "MADS Multitool";
constexpr const char *kAppAgentName = "mads_multitool";
constexpr const char *kSettingsFileName = "mads_multitool_settings.json";

struct AppSettings {
  std::string host = "localhost";
  int port = 9092;
  std::string client_private_key_path;
  std::string broker_public_key_path;
};

struct TopicState {
  std::string payload;
  std::chrono::steady_clock::time_point last_received_at = std::chrono::steady_clock::now();
  bool expanded = false;
  bool json_valid = false;
  json parsed_payload;
};

struct JsonSpan {
  std::string text;
  ImVec4 color;
};

using JsonLine = std::vector<JsonSpan>;

ImVec4 color_text() {
  return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

ImVec4 color_key() {
  return ImVec4(0.95F, 0.77F, 0.35F, 1.0F);
}

ImVec4 color_string() {
  return ImVec4(0.54F, 0.83F, 0.52F, 1.0F);
}

ImVec4 color_number() {
  return ImVec4(0.47F, 0.74F, 0.95F, 1.0F);
}

ImVec4 color_bool() {
  return ImVec4(0.95F, 0.47F, 0.47F, 1.0F);
}

ImVec4 color_null() {
  return ImVec4(0.65F, 0.65F, 0.65F, 1.0F);
}

std::string synthesize_settings_uri(const AppSettings &settings) {
  return "tcp://" + settings.host + ":" + std::to_string(settings.port);
}

std::string read_file(const fs::path &path) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool write_file(const fs::path &path, const std::string &content) {
  std::ofstream output(path, std::ios::trunc);
  if (!output) {
    return false;
  }
  output << content;
  return static_cast<bool>(output);
}

std::optional<AppSettings> load_settings(const fs::path &path) {
  const std::string raw = read_file(path);
  if (raw.empty()) {
    return std::nullopt;
  }

  try {
    const json parsed = json::parse(raw);
    AppSettings settings;
    settings.host = parsed.value("host", settings.host);
    settings.port = parsed.value("port", settings.port);
    settings.client_private_key_path = parsed.value("client_private_key_path", std::string{});
    settings.broker_public_key_path = parsed.value("broker_public_key_path", std::string{});
    return settings;
  } catch (...) {
    return std::nullopt;
  }
}

void save_settings(const fs::path &path, const AppSettings &settings) {
  const json serialized = {
      {"host", settings.host},
      {"port", settings.port},
      {"client_private_key_path", settings.client_private_key_path},
      {"broker_public_key_path", settings.broker_public_key_path},
  };
  write_file(path, serialized.dump(2));
}

std::string format_json(const std::string &raw, bool *valid = nullptr, json *parsed_out = nullptr) {
  try {
    json parsed = json::parse(raw);
    if (valid != nullptr) {
      *valid = true;
    }
    if (parsed_out != nullptr) {
      *parsed_out = parsed;
    }
    return parsed.dump(2);
  } catch (...) {
    if (valid != nullptr) {
      *valid = false;
    }
    if (parsed_out != nullptr) {
      *parsed_out = json();
    }
    return raw;
  }
}

void append_line(std::vector<JsonLine> &lines) {
  lines.emplace_back();
}

void append_span(std::vector<JsonLine> &lines, const std::string &text, ImVec4 color) {
  if (lines.empty()) {
    append_line(lines);
  }
  lines.back().push_back(JsonSpan{text, color});
}

std::string indent_string(int depth) {
  return std::string(static_cast<size_t>(depth) * 2U, ' ');
}

void build_json_lines(const json &value, int depth, std::vector<JsonLine> &lines);

void append_primitive(const json &value, std::vector<JsonLine> &lines) {
  if (value.is_string()) {
    append_span(lines, value.dump(), color_string());
    return;
  }
  if (value.is_number()) {
    append_span(lines, value.dump(), color_number());
    return;
  }
  if (value.is_boolean()) {
    append_span(lines, value.dump(), color_bool());
    return;
  }
  if (value.is_null()) {
    append_span(lines, "null", color_null());
    return;
  }
  append_span(lines, value.dump(), color_text());
}

void build_json_object_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  append_span(lines, "{", color_text());
  if (value.empty()) {
    append_span(lines, "}", color_text());
    return;
  }

  append_line(lines);
  size_t index = 0;
  for (auto it = value.begin(); it != value.end(); ++it, ++index) {
    append_span(lines, indent_string(depth + 1), color_text());
    append_span(lines, json(it.key()).dump(), color_key());
    append_span(lines, ": ", color_text());
    if (it->is_structured()) {
      build_json_lines(*it, depth + 1, lines);
    } else {
      append_primitive(*it, lines);
    }
    if (index + 1 < value.size()) {
      append_span(lines, ",", color_text());
    }
    append_line(lines);
  }
  append_span(lines, indent_string(depth), color_text());
  append_span(lines, "}", color_text());
}

void build_json_array_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  append_span(lines, "[", color_text());
  if (value.empty()) {
    append_span(lines, "]", color_text());
    return;
  }

  append_line(lines);
  for (size_t index = 0; index < value.size(); ++index) {
    append_span(lines, indent_string(depth + 1), color_text());
    if (value[index].is_structured()) {
      build_json_lines(value[index], depth + 1, lines);
    } else {
      append_primitive(value[index], lines);
    }
    if (index + 1 < value.size()) {
      append_span(lines, ",", color_text());
    }
    append_line(lines);
  }
  append_span(lines, indent_string(depth), color_text());
  append_span(lines, "]", color_text());
}

void build_json_lines(const json &value, int depth, std::vector<JsonLine> &lines) {
  if (value.is_object()) {
    build_json_object_lines(value, depth, lines);
    return;
  }
  if (value.is_array()) {
    build_json_array_lines(value, depth, lines);
    return;
  }
  append_primitive(value, lines);
}

void render_json_lines(const std::vector<JsonLine> &lines) {
  for (const JsonLine &line : lines) {
    bool first = true;
    for (const JsonSpan &span : line) {
      if (!first) {
        ImGui::SameLine(0.0F, 0.0F);
      }
      ImGui::PushStyleColor(ImGuiCol_Text, span.color);
      ImGui::TextUnformatted(span.text.c_str());
      ImGui::PopStyleColor();
      first = false;
    }
    if (line.empty()) {
      ImGui::TextUnformatted("");
    }
  }
}

void render_json_view(const json &value, const ImVec2 &size) {
  std::vector<JsonLine> lines;
  lines.reserve(64);
  build_json_lines(value, 0, lines);
  ImGui::BeginChild(ImGui::GetID("json_view"), size, true, ImGuiWindowFlags_HorizontalScrollbar);
  render_json_lines(lines);
  ImGui::EndChild();
}

void render_invalid_json_view(const std::string &raw, const ImVec2 &size) {
  ImGui::BeginChild(ImGui::GetID("json_view_invalid"), size, true, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90F, 0.43F, 0.43F, 1.0F));
  ImGui::TextWrapped("Invalid JSON");
  ImGui::PopStyleColor();
  ImGui::Separator();
  ImGui::TextUnformatted(raw.c_str());
  ImGui::EndChild();
}

ImFont *load_monospace_font(float size_pixels) {
  ImGuiIO &io = ImGui::GetIO();
  const std::array<const char *, 6> candidates = {
      "/System/Library/Fonts/Menlo.ttc",
      "/System/Library/Fonts/SFNSMono.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "C:/Windows/Fonts/consola.ttf",
      "C:/Windows/Fonts/cour.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
  };

  for (const char *candidate : candidates) {
    if (fs::exists(candidate)) {
      if (ImFont *font = io.Fonts->AddFontFromFileTTF(candidate, size_pixels); font != nullptr) {
        return font;
      }
    }
  }
  return io.Fonts->AddFontDefault();
}

class MadsBridge {
public:
  MadsBridge() = default;

  ~MadsBridge() {
    disconnect();
  }

  bool connect(const AppSettings &settings) {
    disconnect();
    clear_error();

    try {
      const bool use_crypto = !settings.client_private_key_path.empty() &&
                              !settings.broker_public_key_path.empty();
      if ((!settings.client_private_key_path.empty() || !settings.broker_public_key_path.empty()) &&
          !use_crypto) {
        set_error("Both key files must be selected to enable CURVE.");
        return false;
      }

      std::map<std::string, std::string> crypto_settings;
      if (use_crypto) {
        const fs::path client_key_path = fs::path(settings.client_private_key_path);
        const fs::path broker_key_path = fs::path(settings.broker_public_key_path);
        if (!fs::exists(client_key_path) || !fs::exists(broker_key_path)) {
          set_error("Selected key file does not exist.");
          return false;
        }
        if (client_key_path.parent_path() != broker_key_path.parent_path()) {
          set_error("MADS expects client and broker keys in the same directory.");
          return false;
        }

        crypto_settings["key_dir"] = client_key_path.parent_path().string();
        crypto_settings["key_client"] = client_key_path.stem().string();
        crypto_settings["key_broker"] = broker_key_path.stem().string();
      }

      agent_ = Mads::start_agent(kAppAgentName, synthesize_settings_uri(settings), crypto_settings);
      agent_->set_high_watermark(1);
      agent_->set_receive_timeout(std::chrono::milliseconds(50));

      stop_requested_.store(false);
      receive_thread_ = std::thread(&MadsBridge::receive_loop, this);
      connected_.store(true);
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      agent_.reset();
      connected_.store(false);
      return false;
    }
  }

  void disconnect() {
    stop_requested_.store(true);
    if (receive_thread_.joinable()) {
      receive_thread_.join();
    }

    if (agent_ != nullptr) {
      try {
        agent_->disconnect();
      } catch (...) {
      }
      agent_.reset();
    }

    connected_.store(false);
  }

  bool is_connected() const {
    return connected_.load();
  }

  bool publish_message(const std::string &topic, const std::string &payload) {
    if (agent_ == nullptr) {
      set_error("Not connected.");
      return false;
    }

    try {
      agent_->publish(json::parse(payload), topic);
      clear_error();
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      return false;
    }
  }

  std::map<std::string, TopicState> snapshot_topics() const {
    std::scoped_lock lock(mutex_);
    return topics_;
  }

  void clear_topics() {
    std::scoped_lock lock(mutex_);
    topics_.clear();
  }

  void set_topic_expanded(const std::string &topic, bool expanded) {
    std::scoped_lock lock(mutex_);
    if (auto it = topics_.find(topic); it != topics_.end()) {
      it->second.expanded = expanded;
    }
  }

  std::string last_error() const {
    std::scoped_lock lock(mutex_);
    return last_error_;
  }

private:
  void receive_loop() {
    while (!stop_requested_.load()) {
      try {
        if (agent_ == nullptr) {
          return;
        }

        const Mads::message_type message_type = agent_->receive(true);
        if (message_type == Mads::message_type::json) {
          const auto [topic, payload] = agent_->last_message();
          bool valid = false;
          json parsed;
          const std::string pretty = format_json(payload, &valid, &parsed);
          {
            std::scoped_lock lock(mutex_);
            TopicState &state = topics_[topic];
            state.payload = pretty;
            state.last_received_at = std::chrono::steady_clock::now();
            state.json_valid = valid;
            state.parsed_payload = valid ? parsed : json();
          }
          clear_error();
          continue;
        }
      } catch (const std::exception &error) {
        set_error(error.what());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  void set_error(std::string message) {
    std::scoped_lock lock(mutex_);
    last_error_ = std::move(message);
  }

  void clear_error() {
    std::scoped_lock lock(mutex_);
    last_error_.clear();
  }

  std::unique_ptr<Mads::Agent> agent_;
  std::thread receive_thread_;
  mutable std::mutex mutex_;
  std::map<std::string, TopicState> topics_;
  std::string last_error_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> stop_requested_{false};
};

struct UiState {
  AppSettings connection_settings;
  MadsBridge bridge;
  fs::path settings_path = fs::current_path() / kSettingsFileName;
  std::string publish_topic;
  std::string publish_buffer = "{\n  \"message\": \"hello\"\n}";
  std::string status_message;
  bool publish_json_valid = true;
  json parsed_publish_json;
  ImFont *monospace_font = nullptr;
  bool settings_dirty = false;
};

void mark_settings_dirty(UiState &ui_state) {
  ui_state.settings_dirty = true;
}

void persist_settings_if_needed(UiState &ui_state) {
  if (!ui_state.settings_dirty) {
    return;
  }
  save_settings(ui_state.settings_path, ui_state.connection_settings);
  ui_state.settings_dirty = false;
}

void draw_connection_bar(UiState &ui_state) {
  AppSettings &settings = ui_state.connection_settings;

  ImGui::SeparatorText("Connection");
  char host_buffer[256];
  std::snprintf(host_buffer, sizeof(host_buffer), "%s", settings.host.c_str());
  if (ImGui::InputText("Host", host_buffer, sizeof(host_buffer))) {
    settings.host = host_buffer;
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  int port = settings.port;
  if (ImGui::InputInt("Port", &port, 0, 0)) {
    settings.port = std::max(port, 0);
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  ImGui::TextDisabled("URI: %s", synthesize_settings_uri(settings).c_str());

  if (ImGui::Button("Client Private Key")) {
    const auto result = pfd::open_file("Select client private key", ".", {"Private key", "*.key"}, pfd::opt::none).result();
    if (!result.empty()) {
      settings.client_private_key_path = result.front();
      mark_settings_dirty(ui_state);
    }
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(settings.client_private_key_path.empty() ? "(none)" : settings.client_private_key_path.c_str());

  if (ImGui::Button("Broker Public Key")) {
    const auto result = pfd::open_file("Select broker public key", ".", {"Public key", "*.pub"}, pfd::opt::none).result();
    if (!result.empty()) {
      settings.broker_public_key_path = result.front();
      mark_settings_dirty(ui_state);
    }
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(settings.broker_public_key_path.empty() ? "(none)" : settings.broker_public_key_path.c_str());

  if (!ui_state.bridge.is_connected()) {
    if (ImGui::Button("Connect")) {
      if (ui_state.bridge.connect(settings)) {
        ui_state.status_message = "Connected.";
      } else {
        ui_state.status_message = "Connect failed.";
      }
      persist_settings_if_needed(ui_state);
    }
  } else if (ImGui::Button("Disconnect")) {
    ui_state.bridge.disconnect();
    ui_state.status_message = "Disconnected.";
  }

  ImGui::SameLine();
  ImGui::TextDisabled("%s", ui_state.bridge.is_connected() ? "online" : "offline");

  const std::string bridge_error = ui_state.bridge.last_error();
  if (!bridge_error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92F, 0.39F, 0.39F, 1.0F));
    ImGui::TextWrapped("%s", bridge_error.c_str());
    ImGui::PopStyleColor();
  } else if (!ui_state.status_message.empty()) {
    ImGui::TextWrapped("%s", ui_state.status_message.c_str());
  }
}

void draw_publish_panel(UiState &ui_state) {
  ImGui::SeparatorText("Publish");

  char topic_buffer[256];
  std::snprintf(topic_buffer, sizeof(topic_buffer), "%s", ui_state.publish_topic.c_str());
  if (ImGui::InputText("Topic", topic_buffer, sizeof(topic_buffer))) {
    ui_state.publish_topic = topic_buffer;
  }

  bool valid = false;
  json parsed;
  const std::string formatted = format_json(ui_state.publish_buffer, &valid, &parsed);
  ui_state.publish_json_valid = valid;
  ui_state.parsed_publish_json = valid ? parsed : json();

  if (ImGui::IsWindowAppearing() && valid) {
    ui_state.publish_buffer = formatted;
  }

  if (ui_state.monospace_font != nullptr) {
    ImGui::PushFont(ui_state.monospace_font);
  }

  std::vector<char> editor_buffer(ui_state.publish_buffer.begin(), ui_state.publish_buffer.end());
  editor_buffer.resize(std::max<size_t>(editor_buffer.size() + 1024U, 4096U), '\0');
  const bool edited = ImGui::InputTextMultiline("##publish_json",
                                                editor_buffer.data(),
                                                editor_buffer.size(),
                                                ImVec2(-FLT_MIN, 180.0F),
                                                ImGuiInputTextFlags_AllowTabInput);
  if (edited) {
    ui_state.publish_buffer = std::string(editor_buffer.data());
  }
  if (ImGui::IsItemDeactivatedAfterEdit()) {
    bool deactivated_valid = false;
    const std::string pretty = format_json(ui_state.publish_buffer, &deactivated_valid);
    if (deactivated_valid) {
      ui_state.publish_buffer = pretty;
    }
  }

  if (ui_state.monospace_font != nullptr) {
    ImGui::PopFont();
  }

  if (!ui_state.publish_json_valid) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92F, 0.39F, 0.39F, 1.0F));
    ImGui::TextUnformatted("JSON is invalid.");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42F, 0.80F, 0.48F, 1.0F));
    ImGui::TextUnformatted("JSON is valid.");
    ImGui::PopStyleColor();
  }

  const bool can_publish = !ui_state.publish_topic.empty() &&
                           !ui_state.publish_buffer.empty() &&
                           ui_state.publish_json_valid &&
                           ui_state.bridge.is_connected();
  if (!can_publish) {
    ImGui::BeginDisabled();
  }
  if (ImGui::Button("Publish")) {
    if (ui_state.bridge.publish_message(ui_state.publish_topic, ui_state.publish_buffer)) {
      ui_state.status_message = "Message published.";
    } else {
      ui_state.status_message = "Publish failed.";
    }
  }
  if (!can_publish) {
    ImGui::EndDisabled();
  }

  ImGui::SeparatorText("Colored Preview");
  if (ui_state.publish_json_valid) {
    render_json_view(ui_state.parsed_publish_json, ImVec2(-FLT_MIN, 220.0F));
  } else {
    render_invalid_json_view(ui_state.publish_buffer, ImVec2(-FLT_MIN, 220.0F));
  }
}

std::string format_elapsed(std::chrono::steady_clock::time_point time_point) {
  const auto elapsed = std::chrono::steady_clock::now() - time_point;
  const double seconds = std::chrono::duration<double>(elapsed).count();
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << seconds << " s";
  return stream.str();
}

void draw_receive_panel(UiState &ui_state) {
  ImGui::SeparatorText("Receive");

  auto topics = ui_state.bridge.snapshot_topics();
  ImGui::BeginChild("receive_topics", ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 2.0F), true);
  for (auto &[topic, state] : topics) {
    const char *toggle_label = state.expanded ? "v" : ">";
    if (ImGui::SmallButton((std::string(toggle_label) + "##" + topic).c_str())) {
      state.expanded = !state.expanded;
      ui_state.bridge.set_topic_expanded(topic, state.expanded);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(topic.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", format_elapsed(state.last_received_at).c_str());

    if (state.expanded) {
      ImGui::Indent();
      if (state.json_valid) {
        render_json_view(state.parsed_payload, ImVec2(-FLT_MIN, 180.0F));
      } else {
        render_invalid_json_view(state.payload, ImVec2(-FLT_MIN, 180.0F));
      }
      ImGui::Unindent();
    }
  }
  ImGui::EndChild();

  if (ImGui::Button("Clear Topics")) {
    ui_state.bridge.clear_topics();
  }
}

} // namespace

int main() {
  if (!glfwInit()) {
    return 1;
  }

#if defined(__APPLE__)
  const char *glsl_version = "#version 150";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
  const char *glsl_version = "#version 130";
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

  GLFWwindow *window = glfwCreateWindow(1440, 900, kWindowTitle, nullptr, nullptr);
  if (window == nullptr) {
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = "imgui.ini";

  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 8.0F;
  style.FrameRounding = 6.0F;
  style.ChildRounding = 6.0F;
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.09F, 0.10F, 0.12F, 1.0F);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.13F, 0.15F, 0.18F, 1.0F);
  style.Colors[ImGuiCol_Button] = ImVec4(0.19F, 0.33F, 0.42F, 1.0F);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25F, 0.44F, 0.56F, 1.0F);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.18F, 0.29F, 0.37F, 1.0F);

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init(glsl_version);

  UiState ui_state;
  if (const auto stored = load_settings(ui_state.settings_path); stored.has_value()) {
    ui_state.connection_settings = *stored;
  }
  ui_state.monospace_font = load_monospace_font(15.0F);

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(io.DisplaySize);

    const ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("MainWindow", nullptr, window_flags);
    draw_connection_bar(ui_state);

    if (ImGui::BeginTable("main_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableNextColumn();
      draw_publish_panel(ui_state);

      ImGui::TableNextColumn();
      draw_receive_panel(ui_state);

      ImGui::EndTable();
    }
    ImGui::End();

    persist_settings_if_needed(ui_state);

    ImGui::Render();
    int display_width = 0;
    int display_height = 0;
    glfwGetFramebufferSize(window, &display_width, &display_height);
    glViewport(0, 0, display_width, display_height);
    glClearColor(0.06F, 0.06F, 0.08F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  ui_state.bridge.disconnect();
  save_settings(ui_state.settings_path, ui_state.connection_settings);

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

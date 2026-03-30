#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>
#include <portable-file-dialogs.h>
#include <TextEditor.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <agent.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
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

constexpr const char *kWindowTitle = "MADS Chat";
constexpr const char *kAppAgentName = "mads_chat";
constexpr const char *kSettingsFileName = "mads_chat_settings.json";

struct TextureHandle {
  unsigned int id = 0;
  int width = 0;
  int height = 0;
};

struct AppSettings {
  std::string host = "localhost";
  int pub_port = 9090;
  int sub_port = 9091;
  std::string subscribe_topics = "";
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

std::string make_endpoint_uri(const std::string &host, int port) {
  return "tcp://" + host + ":" + std::to_string(port);
}

std::string run_command_capture(const char *command) {
#if defined(_WIN32)
  FILE *pipe = _popen(command, "r");
#else
  FILE *pipe = popen(command, "r");
#endif
  if (pipe == nullptr) {
    return {};
  }

  std::array<char, 256> buffer{};
  std::string output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output += buffer.data();
  }

#if defined(_WIN32)
  _pclose(pipe);
#else
  pclose(pipe);
#endif

  while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

TextureHandle load_texture_from_png(const fs::path &path) {
  TextureHandle texture;
  int channels = 0;
  unsigned char *pixels = stbi_load(path.string().c_str(), &texture.width, &texture.height, &channels, STBI_rgb_alpha);
  if (pixels == nullptr) {
    return texture;
  }

  glGenTextures(1, &texture.id);
  glBindTexture(GL_TEXTURE_2D, texture.id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D,
               0,
               GL_RGBA,
               texture.width,
               texture.height,
               0,
               GL_RGBA,
               GL_UNSIGNED_BYTE,
               pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  stbi_image_free(pixels);
  return texture;
}

std::string trim_copy(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> parse_subscribe_topics(const std::string &raw_topics) {
  std::vector<std::string> topics;
  std::stringstream stream(raw_topics);
  std::string item;
  while (std::getline(stream, item, ',')) {
    item = trim_copy(std::move(item));
    if (!item.empty()) {
      topics.push_back(std::move(item));
    }
  }

  if (topics.empty()) {
    topics.push_back("");
  }
  return topics;
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
    settings.sub_port = parsed.value("sub_port", settings.sub_port);
    settings.pub_port = parsed.value("pub_port", settings.pub_port);
    settings.subscribe_topics = parsed.value("subscribe_topics", settings.subscribe_topics);
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
      {"sub_port", settings.sub_port},
      {"pub_port", settings.pub_port},
      {"subscribe_topics", settings.subscribe_topics},
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
    clear_error();

    try {
      const bool use_crypto = !settings.client_private_key_path.empty() &&
                              !settings.broker_public_key_path.empty();
      if ((!settings.client_private_key_path.empty() || !settings.broker_public_key_path.empty()) &&
          !use_crypto) {
        set_error("Both key files must be selected to enable CURVE.");
        return false;
      }
      
      if (_agent == nullptr) {
        _agent = std::make_unique<Mads::Agent>(kAppAgentName, "none");
      }
      
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
        _agent->set_key_dir(client_key_path.parent_path().string());
        _agent->client_key_name = client_key_path.stem().string();
        _agent->server_key_name = broker_key_path.stem().string();
      }

      if (!_initialized) {
        _agent->init();
        _initialized = true;
      }
      _agent->set_sub_topic(parse_subscribe_topics(settings.subscribe_topics));
      _agent->set_sub_endpoint(make_endpoint_uri(settings.host, settings.sub_port));
      _agent->set_pub_endpoint(make_endpoint_uri(settings.host, settings.pub_port));
      _agent->set_receive_timeout(std::chrono::milliseconds(50));
      _agent->connect();

      if (!_receive_thread.joinable()) {
        _stop_requested.store(false);
        _receive_thread = std::thread(&MadsBridge::receive_loop, this);
      }
      _connected.store(true);
      _ever_connected.store(true);
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      _connected.store(false);
      return false;
    }
  }

  void disconnect() {
    _stop_requested.store(true);
    if (_receive_thread.joinable()) {
      _receive_thread.join();
    }

    if (_agent != nullptr) {
      try {
        _agent->disconnect();
      } catch (...) {
      }
      _agent.reset();
    }

    clear_topics();
    _connected.store(false);
    _initialized = false;
  }

  bool is_connected() const {
    return _connected.load();
  }

  bool has_connected_once() const {
    return _ever_connected.load();
  }

  bool publish_message(const std::string &topic, const std::string &payload) {
    if (_agent == nullptr) {
      set_error("Not connected.");
      return false;
    }

    try {
      _agent->publish(json::parse(payload), topic);
      clear_error();
      return true;
    } catch (const std::exception &error) {
      set_error(error.what());
      return false;
    }
  }

  std::map<std::string, TopicState> snapshot_topics() const {
    std::scoped_lock lock(_mutex);
    return _topics;
  }

  void clear_topics() {
    std::scoped_lock lock(_mutex);
    _topics.clear();
  }

  void set_topic_expanded(const std::string &topic, bool expanded) {
    std::scoped_lock lock(_mutex);
    if (auto it = _topics.find(topic); it != _topics.end()) {
      it->second.expanded = expanded;
    }
  }

  std::string last_error() const {
    std::scoped_lock lock(_mutex);
    return _last_error;
  }

private:
  void receive_loop() {
    while (!_stop_requested.load()) {
      try {
        if (_agent == nullptr) {
          return;
        }

        const Mads::message_type message_type = _agent->receive(true);
        if (message_type == Mads::message_type::json) {
          const auto [topic, payload] = _agent->last_message();
          bool valid = false;
          json parsed;
          const std::string pretty = format_json(payload, &valid, &parsed);
          {
            std::scoped_lock lock(_mutex);
            TopicState &state = _topics[topic];
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
    std::scoped_lock lock(_mutex);
    _last_error = std::move(message);
  }

  void clear_error() {
    std::scoped_lock lock(_mutex);
    _last_error.clear();
  }

  std::unique_ptr<Mads::Agent> _agent;
  std::thread _receive_thread;
  mutable std::mutex _mutex;
  std::map<std::string, TopicState> _topics;
  std::string _last_error;
  std::atomic<bool> _connected{false};
  std::atomic<bool> _ever_connected{false};
  std::atomic<bool> _stop_requested{false};
  bool _initialized = false;
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
  TextEditor publish_editor;
  ImFont *monospace_font = nullptr;
  bool settings_dirty = false;
  std::string mads_version;
  std::string mads_prefix;
  TextureHandle logo_texture;
};

void refresh_publish_validation(UiState &ui_state) {
  ui_state.publish_buffer = ui_state.publish_editor.GetText();
  bool valid = false;
  json parsed;
  format_json(ui_state.publish_buffer, &valid, &parsed);
  ui_state.publish_json_valid = valid;
  ui_state.parsed_publish_json = valid ? parsed : json();
}

void configure_publish_editor(UiState &ui_state) {
  ui_state.publish_editor.SetLanguage(TextEditor::Language::Json());
  ui_state.publish_editor.SetTabSize(2);
  ui_state.publish_editor.SetInsertSpacesOnTabs(true);
  ui_state.publish_editor.SetAutoIndentEnabled(true);
  ui_state.publish_editor.SetShowLineNumbersEnabled(false);
  ui_state.publish_editor.SetShowScrollbarMiniMapEnabled(false);
  ui_state.publish_editor.SetShowPanScrollIndicatorEnabled(false);
  ui_state.publish_editor.SetShowMatchingBrackets(true);
  ui_state.publish_editor.SetCompletePairedGlyphs(true);
  ui_state.publish_editor.SetPalette(TextEditor::GetDarkPalette());
  ui_state.publish_editor.SetText(format_json(ui_state.publish_buffer));
  ui_state.publish_editor.SetChangeCallback([&ui_state]() {
    refresh_publish_validation(ui_state);
  }, 100);
  refresh_publish_validation(ui_state);
}

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
  int sub_port = settings.sub_port;
  int pub_port = settings.pub_port;
  char topics_buffer[512];
  std::snprintf(topics_buffer, sizeof(topics_buffer), "%s", settings.subscribe_topics.c_str());

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Host:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220.0F);
  if (ImGui::InputText("##host", host_buffer, sizeof(host_buffer))) {
    settings.host = host_buffer;
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Sub port:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0F);
  if (ImGui::InputInt("##sub_port", &sub_port, 0, 0)) {
    settings.sub_port = std::max(sub_port, 0);
    mark_settings_dirty(ui_state);
  }

  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Pub port:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0F);
  if (ImGui::InputInt("##pub_port", &pub_port, 0, 0)) {
    settings.pub_port = std::max(pub_port, 0);
    mark_settings_dirty(ui_state);
  }

  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Subscribe topics:");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::InputText("##subscribe_topics", topics_buffer, sizeof(topics_buffer))) {
    settings.subscribe_topics = topics_buffer;
    mark_settings_dirty(ui_state);
  }

  ImGui::TextDisabled("Sub URI: %s", make_endpoint_uri(settings.host, settings.sub_port).c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("Pub URI: %s", make_endpoint_uri(settings.host, settings.pub_port).c_str());

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

  const char *connect_label = ui_state.bridge.has_connected_once() ? "Reconnect" : "Connect";
  if (ImGui::Button(connect_label)) {
    const bool was_connected = ui_state.bridge.is_connected();
    if (ui_state.bridge.connect(settings)) {
      ui_state.status_message = was_connected ? "Reconnected." : "Connected.";
    } else {
      ui_state.status_message = was_connected ? "Reconnect failed." : "Connect failed.";
    }
    persist_settings_if_needed(ui_state);
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

  if (ui_state.monospace_font != nullptr) {
    ImGui::PushFont(ui_state.monospace_font);
  }
  ui_state.publish_editor.Render("##publish_json_editor", ImVec2(-FLT_MIN, 260.0F), true);

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

  if (ImGui::Button("Format JSON")) {
    if (ui_state.publish_json_valid) {
      const std::string pretty = format_json(ui_state.publish_buffer);
      ui_state.publish_editor.SetText(pretty);
      refresh_publish_validation(ui_state);
      ui_state.status_message = "JSON formatted.";
    } else {
      ui_state.status_message = "Cannot format invalid JSON.";
    }
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

void draw_status_footer(UiState &ui_state) {
  ImGui::Separator();
  const float line_height = ImGui::GetTextLineHeightWithSpacing();
  const float image_height = line_height * 2.0F;
  const float content_height = std::max(image_height, line_height * 2.0F);
  const float footer_height = content_height + ImGui::GetStyle().FramePadding.y * 4.0F;
  ImGui::BeginChild("status_footer", ImVec2(-FLT_MIN, footer_height), false, ImGuiWindowFlags_NoScrollbar);

  if (ui_state.logo_texture.id != 0 && ui_state.logo_texture.width > 0 && ui_state.logo_texture.height > 0) {
    const float image_width = image_height * static_cast<float>(ui_state.logo_texture.width) /
                              static_cast<float>(ui_state.logo_texture.height);
    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(ui_state.logo_texture.id)),
                 ImVec2(image_width, image_height));
    ImGui::SameLine();
  }

  ImGui::BeginGroup();
  ImGui::Text("Version: %s", ui_state.mads_version.empty() ? "(unavailable)" : ui_state.mads_version.c_str());
  ImGui::Text("Prefix: %s", ui_state.mads_prefix.empty() ? "(unavailable)" : ui_state.mads_prefix.c_str());
  ImGui::EndGroup();

  ImGui::EndChild();
}

float get_status_footer_height() {
  const float line_height = ImGui::GetTextLineHeightWithSpacing();
  const float image_height = line_height * 2.0F;
  const float content_height = std::max(image_height, line_height * 2.0F);
  return content_height + ImGui::GetStyle().FramePadding.y * 4.0F + ImGui::GetStyle().ItemSpacing.y;
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
  ui_state.mads_version = run_command_capture("mads --version");
  ui_state.mads_prefix = run_command_capture("mads --prefix");
  if (!ui_state.mads_prefix.empty()) {
    ui_state.logo_texture = load_texture_from_png(fs::path(ui_state.mads_prefix) / "share/images/logo_white.png");
  }
  configure_publish_editor(ui_state);

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

    const float footer_reserved_height = get_status_footer_height();
    if (ImGui::BeginChild("content_area", ImVec2(-FLT_MIN, -footer_reserved_height), false)) {
      if (ImGui::BeginTable("main_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        draw_publish_panel(ui_state);

        ImGui::TableNextColumn();
        draw_receive_panel(ui_state);

        ImGui::EndTable();
      }
    }
    ImGui::EndChild();

    draw_status_footer(ui_state);
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
  if (ui_state.logo_texture.id != 0) {
    glDeleteTextures(1, &ui_state.logo_texture.id);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

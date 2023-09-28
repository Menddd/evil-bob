#include <spud/detour.h>

#include <il2cpp/il2cpp_helper.h>

#include <prime/EntityGroup.h>
#include <prime/HttpResponse.h>
#include <prime/Hub.h>
#include <prime/ServiceResponse.h>

#include <prime/proto/Digit.PrimeServer.Models.pb.h>

#include "config.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <condition_variable>
#include <format>
#include <queue>
#include <string>

namespace http
{
using namespace winrt;
using namespace Windows::Foundation;

static auto get_client(std::wstring sessionid = L"")
{
  Windows::Web::Http::HttpClient httpClient;
  auto                           headers{httpClient.DefaultRequestHeaders()};
  std::wstring                   header = L"stfc community patch";
  if (!headers.UserAgent().TryParseAdd(header)) {
    throw L"Invalid header value: " + header;
  }
  auto token = to_hstring(Config::Get().sync_token);
  if (!token.empty() && sessionid.empty()) {
    headers.Append(L"stfc-sync-token", token);
  }
  if (!sessionid.empty()) {
    headers.Append(L"X-AUTH-SESSION-ID", sessionid);
    headers.Append(L"X-PRIME-VERSION", L"meh");
    headers.Append(L"X-Api-Key", L"meh");
  }
  return httpClient;
}

static void send_data(std::wstring post_data)
{
  if (Config::Get().sync_url.empty()) {
    return;
  }

  using namespace Windows::Storage::Streams;
  try {
    Windows::Web::Http::HttpResponseMessage httpResponseMessage;
    std::wstring                            httpResponseBody;

    auto httpClient = get_client();
    Uri  requestUri{winrt::to_hstring(Config::Get().sync_url)};

    Windows::Web::Http::HttpStringContent jsonContent(post_data, UnicodeEncoding::Utf8, L"application/json");
    httpResponseMessage = httpClient.PostAsync(requestUri, jsonContent).get();
    httpResponseMessage.EnsureSuccessStatusCode();
  } catch (winrt::hresult_error const& ex) {
    spdlog::error("Failed to send sync data: {}", winrt::to_string(ex.message()).c_str());
  }
}

static std::wstring get_data_data(std::wstring session, std::wstring url, std::wstring path, std::wstring post_data)
{
  if (Config::Get().sync_url.empty()) {
    return {};
  }

  using namespace Windows::Storage::Streams;
  try {
    Windows::Web::Http::HttpResponseMessage httpResponseMessage;
    std::wstring                            httpResponseBody;

    auto httpClient = get_client(session);
    if (url.ends_with(L"/")) {
      url = url.substr(0, url.length() - 1);
      url += path;
    } else {
      url += path;
    }
    Uri requestUri{url};

    Windows::Web::Http::HttpStringContent jsonContent(post_data, UnicodeEncoding::Utf8, L"application/json");
    httpResponseMessage = httpClient.PostAsync(requestUri, jsonContent).get();
    httpResponseMessage.EnsureSuccessStatusCode();
    return httpResponseMessage.Content().ReadAsStringAsync().get().c_str();
  } catch (winrt::hresult_error const& ex) {
    spdlog::error("Failed to send sync data: {}", winrt::to_string(ex.message()).c_str());
  }
}

static void send_data(std::string post_data)
{
  return send_data(std::wstring(winrt::to_hstring(post_data)));
}

} // namespace http

std::mutex              m;
std::condition_variable cv;
std::queue<std::string> sync_data_queue;

std::mutex              m2;
std::condition_variable cv2;
std::queue<uint64_t>    combat_log_data_queue;

void queue_data(std::string data)
{
  if (data == "[]")
    return;

  {
    std::lock_guard lk(m);
    sync_data_queue.push(data);
  }
  cv.notify_all();
}

void HandleEntityGroup(EntityGroup* entity_group);

void MissionsDataContainer_ParseBinaryObject(auto original, void* _this, EntityGroup* group, bool isPlayerData)
{
  HandleEntityGroup(group);
  return original(_this, group, isPlayerData);
}

void GameServerModelRegistry_ProcessResultInternal(auto original, void* _this, HttpResponse* http_response,
                                                   ServiceResponse* service_response, void* callback,
                                                   void* callback_error)
{
  auto entity_groups = service_response->EntityGroups;
  for (int i = 0; i < entity_groups->Count; ++i) {
    auto entity_group = entity_groups->get_Item(i);
    HandleEntityGroup(entity_group);
  }

  return original(_this, http_response, service_response, callback, callback_error);
}
void GameServerModelRegistry_HandleBinaryObjects(auto original, void* _this, ServiceResponse* service_response)
{
  auto entity_groups = service_response->EntityGroups;
  for (int i = 0; i < entity_groups->Count; ++i) {
    auto entity_group = entity_groups->get_Item(i);
    HandleEntityGroup(entity_group);
  }

  return original(_this, service_response);
}

struct ResourceState {
  ResourceState(int64_t amount = -1)
      : amount(amount)
  {
  }

  inline operator int64_t() const
  {
    return amount;
  }

private:
  int64_t amount = -1;
};

struct RankLevelState {
  RankLevelState(int64_t a = -1, int64_t b = -1)
      : a(a)
      , b(b)
  {
  }

  bool operator==(const RankLevelState& other) const
  {
    return this->a == other.a && this->b == other.b;
  }

private:
  int64_t a = -1;
  int64_t b = -1;
};

struct pairhash {
public:
  template <typename T, typename U> std::size_t operator()(const std::pair<T, U>& x) const
  {
    return std::hash<T>()(x.first) ^ std::hash<U>()(x.second);
  }
};

static std::unordered_map<uint64_t, ResourceState>                               resource_states;
static std::unordered_map<uint64_t, ResourceState>                               module_states;
static std::unordered_map<uint64_t, RankLevelState>                              officer_states;
static std::unordered_map<uint64_t, RankLevelState>                              ft_states;
static std::unordered_map<std::pair<int64_t, int64_t>, RankLevelState, pairhash> trait_states;
static std::unordered_set<int64_t>                                               mission_states;
static std::unordered_set<uint64_t>                                              battlelog_states;

void HandleEntityGroup(EntityGroup* entity_group)
{
  using json = nlohmann::json;

  auto bytes = entity_group->Group;
  auto type  = entity_group->Type_;
  if (type == EntityGroup::Type::CompletedMissions) {
    auto response = Digit::PrimeServer::Models::CompletedMissionsResponse();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto mission_array = json::array();
      for (const auto& mission : response.completedmissions()) {
        if (!mission_states.contains(mission)) {
          mission_states.insert(mission);
          mission_array.push_back({{"type", "mission"}, {"mid", mission}});
        }
      }
      queue_data(mission_array.dump());
    }
  } else if (type == EntityGroup::Type::PlayerInventories) {
    auto response = Digit::PrimeServer::Models::InventoryResponse();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto inventory_object = json();
      for (const auto& inventory : response.inventories()) {
        for (const auto& item : inventory.second.items()) {
          auto type = item.type();
          if (type == Digit::PrimeServer::Models::INVENTORYITEMTYPE_INVENTORYRESOURCE)
            inventory_object[std::to_string(item.commonparams().refid())] = item.count();
        }
      }
    }
  } else if (type == EntityGroup::Type::ResearchTreesState) {
    auto response = Digit::PrimeServer::Models::ResearchTreesState();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto research_array = json::array();
      for (const auto& research : response.researchprojectlevels()) {
        research_array.push_back({{"type", "research"}, {"rid", research.first}, {"level", research.second}});
      }
      queue_data(research_array.dump());
    }
  } else if (type == EntityGroup::Type::Officers) {
    auto response = Digit::PrimeServer::Models::OfficersResponse();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto officers_array = json::array();
      for (const auto& officer : response.officers()) {
        if (officer.rankindex() == 0) {
          continue;
        }
        if (officer_states[officer.id()] != RankLevelState{officer.rankindex(), officer.level()}) {
          officer_states[officer.id()] = RankLevelState{officer.rankindex(), officer.level()};
          officers_array.push_back(
              {{"type", "officer"}, {"oid", officer.id()}, {"rank", officer.rankindex()}, {"level", officer.level()}});
        }
      }
      queue_data(officers_array.dump());
    }
  } else if (type == EntityGroup::Type::ForbiddenTechs) {
    auto response = Digit::PrimeServer::Models::ForbiddenTechsResponse();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto ft_array = json::array();
      for (const auto& ft : response.forbiddentechs()) {
        if (ft_states[ft.id()] != RankLevelState{ft.tier(), ft.level()}) {
          ft_states[ft.id()] = RankLevelState{ft.tier(), ft.level()};
          ft_array.push_back({{"type", "ft"}, {"fid", ft.id()}, {"tier", ft.tier()}, {"level", ft.level()}});
        }
      }
      queue_data(ft_array.dump());
    }
  } else if (type == EntityGroup::Type::ActiveOfficerTraits) {
    auto response = Digit::PrimeServer::Models::ActiveOfficerTraits();
    if (response.ParseFromArray(bytes->bytes->m_Items, bytes->bytes->max_length)) {
      auto trait_array = json::array();
      for (const auto& trait : response.activetraits()) {
        const auto key = std::make_pair(response.officerid(), trait.first);
        if (trait_states[key] != trait.second.level()) {
          trait_states[key] = trait.second.level();
          trait_array.push_back({{"type", "trait"},
                                 {"oid", response.officerid()},
                                 {"tid", trait.first},
                                 {"level", trait.second.level()}});
        }
      }
      queue_data(trait_array.dump());
    }
  } else if (type == EntityGroup::Type::Json) {
    try {
      using json = nlohmann::json;

      auto text   = bytes->bytes->m_Items;
      auto result = json::parse(text);

      if (result.contains("battle_result_headers")) {
        auto headers = result["battle_result_headers"];
        if (battlelog_states.empty()) {
          for (const auto header : headers) {
            const auto id = header["id"].get<uint64_t>();
            battlelog_states.insert(id);
          }
        } else {
          std::lock_guard lk(m2);
          for (const auto header : headers) {
            const auto id = header["id"].get<uint64_t>();
            if (!battlelog_states.contains(id)) {
              battlelog_states.insert(id);
              combat_log_data_queue.push(id);
            }
          }
        }
        cv2.notify_all();
      }

      if (result.contains("resources")) {
        auto resource_array = json::array();
        for (const auto& resource : result["resources"].get<json::object_t>()) {
          auto id     = std::stoll(resource.first);
          auto amount = resource.second["current_amount"].get<int64_t>();
          if (amount == 0) {
            continue;
          }
          if (resource_states[id] != amount) {
            resource_states[id] = amount;
            resource_array.push_back({{"type", "resource"}, {"rid", id}, {"amount", amount}});
          }
        }
        queue_data(resource_array.dump());
      }
      if (result.contains("starbase_modules")) {
        auto starbase_array = json::array();
        for (const auto& resource : result["starbase_modules"].get<json::object_t>()) {
          auto id    = resource.second["id"].get<uint64_t>();
          auto level = resource.second["level"].get<int64_t>();
          if (module_states[id] != level) {
            module_states[id] = level;
            starbase_array.push_back({{"type", "module"}, {"bid", id}, {"level", level}});
          }
        }
        queue_data(starbase_array.dump());
      }
      if (result.contains("ships")) {
        auto ship_array = json::array();
        for (const auto& resource : result["ships"].get<json::object_t>()) {
          ship_array.push_back({{"type", "ship"},
                                {"psid", resource.second["id"]},
                                {"level", resource.second["level"]},
                                {"tier", resource.second["tier"]},
                                {"hull_id", resource.second["hull_id"]},
                                {"components", resource.second["components"]}});
        }
        queue_data(ship_array.dump());
      }
    } catch (json::exception e) {
      printf("Fuck this: %s\n", e.what());
    }
  }
}

static std::wstring instanceSessionId;
static std::wstring gameServerUrl;

void ship_sync_data()
{
  winrt::init_apartment();

  for (;;) {
    {
      std::unique_lock lk(m);
      cv.wait(lk, []() { return !sync_data_queue.empty(); });
    }
    const auto sync_data = ([&] {
      std::lock_guard lk(m);
      auto            data = sync_data_queue.front();
      sync_data_queue.pop();
      return data;
    })();
    try {
      http::send_data(sync_data);
    } catch (winrt::hresult_error const& ex) {
      spdlog::error("Failed to send sync data: {}", winrt::to_string(ex.message()).c_str());
    } catch (const std::wstring& sz) {
      spdlog::error("Failed to send sync data: {}", winrt::to_string(sz).c_str());
    }
  }
}

void ship_combat_log_data()
{
  winrt::init_apartment();

  for (;;) {
    {
      std::unique_lock lk(m2);
      cv2.wait(lk, []() { return !combat_log_data_queue.empty(); });
    }

    const auto sync_data  = ([&] {
      std::lock_guard lk(m2);
      auto            data = combat_log_data_queue.front();
      combat_log_data_queue.pop();
      return data;
    })();
    auto       body       = std::format(L"{{\"journal_id\":{}}}", sync_data);
    auto       battle_log = http::get_data_data(instanceSessionId, gameServerUrl, L"/journals/get", body);

    using json = nlohmann::json;

    auto ship_array  = json::array();
    auto battle_json = json::parse(battle_log);
    ship_array.push_back({{"type", "battlelog"}, {"journal", battle_json["journal"]}});

    try {
      http::send_data(ship_array.dump());
    } catch (winrt::hresult_error const& ex) {
      spdlog::error("Failed to send sync data: {}", winrt::to_string(ex.message()).c_str());
    } catch (const std::wstring& sz) {
      spdlog::error("Failed to send sync data: {}", winrt::to_string(sz).c_str());
    }
  }
}

void PrimeApp_InitPrimeServer(auto original, void* _this, Il2CppString* gameServerUrl, Il2CppString* gatewayServerUrl,
                              Il2CppString* sessionId)
{
  original(_this, gameServerUrl, gatewayServerUrl, sessionId);
  ::instanceSessionId = sessionId->chars;
  ::gameServerUrl     = gameServerUrl->chars;
}

void InstallSyncPatches()
{
  auto missions_data_container =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Models", "MissionsDataContainer");
  auto ptr = missions_data_container.GetMethod("ParseBinaryObject");
  SPUD_STATIC_DETOUR(ptr, MissionsDataContainer_ParseBinaryObject);

  auto inventory_data_container =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Services", "InventoryDataContainer");
  ptr = inventory_data_container.GetMethod("ParseBinaryObject");
  SPUD_STATIC_DETOUR(ptr, MissionsDataContainer_ParseBinaryObject);

  auto research_data_container =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Services", "ResearchDataContainer");
  ptr = research_data_container.GetMethod("ParseBinaryObject");
  SPUD_STATIC_DETOUR(ptr, MissionsDataContainer_ParseBinaryObject);

  auto research_service =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Services", "ResearchService");
  ptr = research_service.GetMethod("ParseBinaryObject");
  SPUD_STATIC_DETOUR(ptr, MissionsDataContainer_ParseBinaryObject);

  auto game_server_model_registry =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimeServer.Core", "GameServerModelRegistry");
  ptr = game_server_model_registry.GetMethod("ProcessResultInternal");
  SPUD_STATIC_DETOUR(ptr, GameServerModelRegistry_ProcessResultInternal);
  ptr = game_server_model_registry.GetMethod("HandleBinaryObjects");
  SPUD_STATIC_DETOUR(ptr, GameServerModelRegistry_HandleBinaryObjects);

  auto platform_model_registry =
      il2cpp_get_class_helper("Digit.Client.PrimeLib.Runtime", "Digit.PrimePlatform.Core", "PlatformModelRegistry");
  ptr = platform_model_registry.GetMethod("ProcessResultInternal");
  SPUD_STATIC_DETOUR(ptr, GameServerModelRegistry_ProcessResultInternal);

  auto authentication_service = il2cpp_get_class_helper("Assembly-CSharp", "Digit.Client.Core", "PrimeApp");
  ptr                         = authentication_service.GetMethod("InitPrimeServer");
  SPUD_STATIC_DETOUR(ptr, PrimeApp_InitPrimeServer);

  std::thread(ship_sync_data).detach();
  std::thread(ship_combat_log_data).detach();
}
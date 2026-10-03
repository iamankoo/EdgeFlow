#include "edgeflow/network/RegistryRequestHandler.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace edgeflow::network {

namespace {

using discovery::HealthStatus;
using discovery::InstanceStatus;
using discovery::InstanceUpdate;
using discovery::NewInstance;
using discovery::RegistryError;
using discovery::RegistryErrorCode;
using discovery::ServiceInstance;
using nlohmann::json;

constexpr std::string_view kServicesRoot = "/services";

std::string dump(const json& value) {
  return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

HttpResponse jsonResponse(const HttpRequest& request, http::status status, const json& body) {
  return makeResponse(request, status, kJsonContentType, dump(body));
}

HttpResponse errorResponse(const HttpRequest& request, http::status status,
                           std::string_view detail) {
  return makeErrorResponse(request.version(), request.keep_alive(), status, detail);
}

HttpResponse methodNotAllowed(const HttpRequest& request, std::string_view allow) {
  auto response = errorResponse(request, http::status::method_not_allowed,
                                std::string{request.method_string()} + " is not allowed here");
  response.set(http::field::allow, allow);
  return response;
}

http::status statusFor(RegistryErrorCode code) {
  switch (code) {
    case RegistryErrorCode::InvalidArgument: return http::status::bad_request;
    case RegistryErrorCode::ServiceNotFound:
    case RegistryErrorCode::InstanceNotFound: return http::status::not_found;
    case RegistryErrorCode::DuplicateInstance: return http::status::conflict;
    case RegistryErrorCode::DatabaseUnavailable: return http::status::service_unavailable;
    case RegistryErrorCode::Internal: return http::status::internal_server_error;
  }
  return http::status::internal_server_error;
}

HttpResponse fromError(const HttpRequest& request, const RegistryError& error) {
  return errorResponse(request, statusFor(error.code), error.message);
}

json toJson(const ServiceInstance& instance) {
  return {{"service", instance.service},
          {"instance_id", instance.instance_id},
          {"host", instance.host},
          {"port", instance.port},
          {"status", std::string{discovery::toString(instance.status)}},
          {"health_status", std::string{discovery::toString(instance.health)}},
          {"version", instance.version},
          {"weight", instance.weight},
          {"connection_count", instance.connection_count},
          {"registered_at", instance.registered_at},
          {"updated_at", instance.updated_at}};
}

// --- request body helpers ---------------------------------------------------------

// Parses a JSON object body; on failure returns the reason.
std::optional<std::string> parseObject(const HttpRequest& request, json& out) {
  out = json::parse(request.body(), nullptr, /*allow_exceptions=*/false);
  if (out.is_discarded()) return "request body is not valid JSON";
  if (!out.is_object()) return "request body must be a JSON object";
  return std::nullopt;
}

std::optional<std::string> rejectUnknownFields(const json& body,
                                               std::initializer_list<std::string_view> allowed,
                                               std::initializer_list<std::string_view> immutable) {
  for (const auto& [key, value] : body.items()) {
    (void)value;
    bool known = false;
    for (const auto name : allowed) known = known || key == name;
    if (known) continue;
    for (const auto name : immutable) {
      if (key == name) return "field '" + key + "' is immutable";
    }
    return "unknown field '" + key + "'";
  }
  return std::nullopt;
}

// Reads an integer field into [min, max]. Absent -> nullopt without error.
std::optional<std::string> readInteger(const json& body, const char* key, std::int64_t min,
                                       std::int64_t max, std::optional<std::int64_t>& out) {
  const auto it = body.find(key);
  if (it == body.end()) return std::nullopt;
  const bool fits = it->is_number_integer() &&
                    (!it->is_number_unsigned() ||
                     it->get<std::uint64_t>() <=
                         static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
  if (!fits) return std::string{"field '"} + key + "' must be an integer";
  const auto value = it->get<std::int64_t>();
  if (value < min || value > max) {
    return std::string{"field '"} + key + "' must be between " + std::to_string(min) + " and " +
           std::to_string(max);
  }
  out = value;
  return std::nullopt;
}

std::optional<std::string> readString(const json& body, const char* key,
                                      std::optional<std::string>& out) {
  const auto it = body.find(key);
  if (it == body.end()) return std::nullopt;
  if (!it->is_string()) return std::string{"field '"} + key + "' must be a string";
  out = it->get<std::string>();
  return std::nullopt;
}

std::optional<std::string> readStatus(const json& body, std::optional<InstanceStatus>& out) {
  std::optional<std::string> text;
  if (auto error = readString(body, "status", text)) return error;
  if (!text) return std::nullopt;
  out = discovery::parseInstanceStatus(*text);
  if (!out) return "field 'status' must be one of active, draining, disabled";
  return std::nullopt;
}

std::optional<std::string> readHealth(const json& body, std::optional<HealthStatus>& out) {
  std::optional<std::string> text;
  if (auto error = readString(body, "health_status", text)) return error;
  if (!text) return std::nullopt;
  out = discovery::parseHealthStatus(*text);
  if (!out) return "field 'health_status' must be one of unknown, healthy, unhealthy";
  return std::nullopt;
}

struct Parsed {
  std::optional<std::string> error;
  NewInstance instance;
};

Parsed parseNewInstance(const HttpRequest& request, std::string service) {
  Parsed parsed;
  parsed.instance.service = std::move(service);
  json body;
  if (auto error = parseObject(request, body)) {
    parsed.error = std::move(error);
    return parsed;
  }
  if (auto error = rejectUnknownFields(body,
                                       {"instance_id", "host", "port", "status", "health_status",
                                        "version", "weight", "connection_count"},
                                       {"service"})) {
    parsed.error = std::move(error);
    return parsed;
  }

  std::optional<std::string> host;
  if (auto error = readString(body, "host", host)) { parsed.error = error; return parsed; }
  if (!host) { parsed.error = "field 'host' is required"; return parsed; }
  parsed.instance.host = *host;

  std::optional<std::int64_t> port;
  if (auto error = readInteger(body, "port", 0, 65535, port)) { parsed.error = error; return parsed; }
  if (!port) { parsed.error = "field 'port' is required"; return parsed; }
  parsed.instance.port = static_cast<std::uint16_t>(*port);

  if (auto error = readString(body, "instance_id", parsed.instance.instance_id)) {
    parsed.error = error;
    return parsed;
  }
  std::optional<std::string> version;
  if (auto error = readString(body, "version", version)) { parsed.error = error; return parsed; }
  if (version) parsed.instance.version = *version;

  std::optional<std::int64_t> weight;
  if (auto error = readInteger(body, "weight", 0, 1000, weight)) { parsed.error = error; return parsed; }
  if (weight) parsed.instance.weight = static_cast<std::uint32_t>(*weight);

  std::optional<std::int64_t> connections;
  if (auto error = readInteger(body, "connection_count", 0, std::numeric_limits<std::int64_t>::max(),
                               connections)) {
    parsed.error = error;
    return parsed;
  }
  if (connections) parsed.instance.connection_count = static_cast<std::uint64_t>(*connections);

  std::optional<InstanceStatus> status;
  if (auto error = readStatus(body, status)) { parsed.error = error; return parsed; }
  if (status) parsed.instance.status = *status;
  std::optional<HealthStatus> health;
  if (auto error = readHealth(body, health)) { parsed.error = error; return parsed; }
  if (health) parsed.instance.health = *health;
  return parsed;
}

struct ParsedUpdate {
  std::optional<std::string> error;
  InstanceUpdate update;
};

ParsedUpdate parseUpdate(const HttpRequest& request) {
  ParsedUpdate parsed;
  json body;
  if (auto error = parseObject(request, body)) {
    parsed.error = std::move(error);
    return parsed;
  }
  if (auto error = rejectUnknownFields(body,
                                       {"status", "health_status", "version", "weight",
                                        "connection_count"},
                                       {"service", "instance_id", "host", "port"})) {
    parsed.error = std::move(error);
    return parsed;
  }
  if (auto error = readStatus(body, parsed.update.status)) { parsed.error = error; return parsed; }
  if (auto error = readHealth(body, parsed.update.health)) { parsed.error = error; return parsed; }
  if (auto error = readString(body, "version", parsed.update.version)) {
    parsed.error = error;
    return parsed;
  }
  std::optional<std::int64_t> weight;
  if (auto error = readInteger(body, "weight", 0, 1000, weight)) { parsed.error = error; return parsed; }
  if (weight) parsed.update.weight = static_cast<std::uint32_t>(*weight);
  std::optional<std::int64_t> connections;
  if (auto error = readInteger(body, "connection_count", 0, std::numeric_limits<std::int64_t>::max(),
                               connections)) {
    parsed.error = error;
    return parsed;
  }
  if (connections) parsed.update.connection_count = static_cast<std::uint64_t>(*connections);
  return parsed;
}

// --- routing ----------------------------------------------------------------------

std::string_view pathOf(std::string_view target) {
  const auto query = target.find('?');
  return query == std::string_view::npos ? target : target.substr(0, query);
}

bool isRegistryPath(std::string_view path) {
  return path == kServicesRoot ||
         (path.size() > kServicesRoot.size() && path.substr(0, kServicesRoot.size()) == kServicesRoot &&
          path[kServicesRoot.size()] == '/');
}

std::vector<std::string_view> split(std::string_view path) {
  std::vector<std::string_view> segments;
  path.remove_prefix(1);  // leading '/'
  while (true) {
    const auto slash = path.find('/');
    segments.push_back(path.substr(0, slash));
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return segments;
}

}  // namespace

RegistryRequestHandler::RegistryRequestHandler(std::shared_ptr<discovery::ServiceRegistry> registry,
                                               std::shared_ptr<RequestHandler> next,
                                               std::shared_ptr<logging::Logger> logger)
    : registry_(std::move(registry)), next_(std::move(next)), logger_(std::move(logger)) {}

HttpResponse RegistryRequestHandler::handle(const HttpRequest& request) {
  const std::string_view path = pathOf(request.target());
  if (!isRegistryPath(path)) return next_->handle(request);

  const auto segments = split(path);  // ["services", ...]
  const auto method = request.method();

  // /services
  if (segments.size() == 1) {
    if (method != http::verb::get) return methodNotAllowed(request, "GET");
    auto services = registry_->listServices();
    if (!services) return fromError(request, services.error());
    return jsonResponse(request, http::status::ok, {{"services", services.value()}});
  }

  // /services/{service}/routable: the instances routing may use (active AND healthy).
  if (segments.size() == 3 && segments[2] == "routable" && !segments[1].empty()) {
    if (method != http::verb::get) return methodNotAllowed(request, "GET");
    const std::string service{segments[1]};
    auto instances = registry_->lookupRoutable(service);
    if (!instances) return fromError(request, instances.error());
    json list = json::array();
    for (const auto& instance : instances.value()) list.push_back(toJson(instance));
    return jsonResponse(request, http::status::ok,
                        {{"service", service}, {"count", list.size()}, {"instances", list}});
  }

  // /services/{service}/instances[/{instance}]
  if (segments.size() < 3 || segments.size() > 4 || segments[2] != "instances" ||
      segments[1].empty() || (segments.size() == 4 && segments[3].empty())) {
    return errorResponse(request, http::status::not_found,
                         "no route for " + std::string{request.method_string()} + " " +
                             std::string{path});
  }
  const std::string service{segments[1]};

  if (segments.size() == 3) {
    if (method == http::verb::get) {
      auto instances = registry_->lookupService(service);
      if (!instances) return fromError(request, instances.error());
      json list = json::array();
      for (const auto& instance : instances.value()) list.push_back(toJson(instance));
      return jsonResponse(request, http::status::ok,
                          {{"service", service}, {"count", list.size()}, {"instances", list}});
    }
    if (method == http::verb::post) {
      auto parsed = parseNewInstance(request, service);
      if (parsed.error) {
        logger_->info("registry API: rejected registration for {}: {}", service, *parsed.error);
        return errorResponse(request, http::status::bad_request, *parsed.error);
      }
      auto stored = registry_->registerInstance(parsed.instance);
      if (!stored) return fromError(request, stored.error());
      auto response = jsonResponse(request, http::status::created, toJson(stored.value()));
      response.set(http::field::location,
                   "/services/" + service + "/instances/" + stored.value().instance_id);
      return response;
    }
    return methodNotAllowed(request, "GET, POST");
  }

  const std::string instance_id{segments[3]};
  if (method == http::verb::get) {
    auto instance = registry_->getInstance(service, instance_id);
    if (!instance) return fromError(request, instance.error());
    return jsonResponse(request, http::status::ok, toJson(instance.value()));
  }
  if (method == http::verb::patch) {
    auto parsed = parseUpdate(request);
    if (parsed.error) {
      logger_->info("registry API: rejected update of {}/{}: {}", service, instance_id,
                    *parsed.error);
      return errorResponse(request, http::status::bad_request, *parsed.error);
    }
    auto updated = registry_->updateInstance(service, instance_id, parsed.update);
    if (!updated) return fromError(request, updated.error());
    return jsonResponse(request, http::status::ok, toJson(updated.value()));
  }
  if (method == http::verb::delete_) {
    auto removed = registry_->deregisterInstance(service, instance_id);
    if (!removed) return fromError(request, removed.error());
    auto response = makeResponse(request, http::status::no_content, kJsonContentType, {});
    response.erase(http::field::content_type);
    return response;
  }
  return methodNotAllowed(request, "GET, PATCH, DELETE");
}

}  // namespace edgeflow::network

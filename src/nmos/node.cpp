#include "nmos/node.hpp"

#include "nmos/ids.hpp"
#include "util/httpclient.hpp"
#include "util/logging.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(SCOPES_WITH_NMOS)
#include "cpprest/host_utils.h"
#include "nmos/capabilities.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/slog.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "sdp/json.h"
#endif

namespace scopes
{
struct NmosNode::Impl
{
    Config config;
    RouteFn route;
    NmosIds ids;
    std::atomic<bool> running{false};
    std::mutex readyMu;
    std::condition_variable readyCv;
    bool ready = false;
    // The resources are in the model; `ready` follows once the listeners are open too.
    bool modelReady = false;
    std::string error;
    // Work for the node thread, applied under the model's write lock.
    std::mutex pendingMu;
    struct SubUpdate
    {
        std::string sender;
        bool active = false;
    };
    std::vector<SubUpdate> subscriptions;
    std::optional<std::pair<OutputFlow, std::string>> flow;
#if defined(SCOPES_WITH_NMOS)
    std::thread thread;
    nmos::node_model* model = nullptr;
#endif

    Impl(Config cfg, RouteFn routeIn)
        : config(std::move(cfg))
        , route(std::move(routeIn))
        , ids(makeNmosIds(config.nmosSeed))
    {
    }
};

NmosNode::NmosNode(Config config, RouteFn route)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(route)))
{
}

NmosNode::~NmosNode()
{
    stop();
}

bool NmosNode::registered() const
{
    if (!impl_->config.nmosEnable)
    {
        return true;
    }
    if (impl_->config.nmosRegistryAddress.empty())
    {
        return impl_->running.load();
    }
    auto const url = "http://" + impl_->config.nmosQueryAddress + ":" + std::to_string(impl_->config.nmosQueryPort) + "/x-nmos/query/v1.3/nodes/" +
                     impl_->ids.node;
    return httpGet(url, 700).status == 200;
}

void NmosNode::setOutputFlow(OutputFlow const& flow, std::string const& domainId)
{
    std::lock_guard lock{impl_->pendingMu};
    impl_->flow = std::make_pair(flow, domainId);
}

void NmosNode::restoreRoute(Route const& route)
{
#if defined(SCOPES_WITH_NMOS)
    if (impl_->model != nullptr)
    {
        auto const text = [](std::string const& value) {
            return value.empty() ? web::json::value::null() : web::json::value::string(utility::conversions::to_string_t(value));
        };
        auto lock = impl_->model->write_lock();
        nmos::modify_resource(impl_->model->connection_resources, utility::conversions::to_string_t(impl_->ids.receiver), [&](nmos::resource& connection) {
            for (auto const* key : {U("active"), U("staged")})
            {
                auto& document = connection.data[key];
                document[U("master_enable")] = web::json::value::boolean(route.enable);
                document[U("sender_id")] = text(route.senderId);
                document[U("transport_params")][0][U("mxl_domain_id")] = text(route.domainId);
                document[U("transport_params")][0][U("mxl_flow_id")] = text(route.flowId);
            }
        });
        impl_->model->notify();
    }
#endif
    std::lock_guard lock{impl_->pendingMu};
    impl_->subscriptions.push_back(Impl::SubUpdate{route.senderId, route.enable && !route.senderId.empty()});
}

#if !defined(SCOPES_WITH_NMOS)
void NmosNode::start()
{
    if (impl_->config.nmosEnable)
    {
        throw std::runtime_error("this binary was built without nmos-cpp");
    }
}

void NmosNode::stop() {}
#else
namespace
{
utility::string_t us(std::string const& text)
{
    return utility::conversions::to_string_t(text);
}

std::string su(utility::string_t const& text)
{
    return utility::conversions::to_utf8string(text);
}

void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
{
    if (!resource.data.has_field(U("tags")))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
}

void tagMode(nmos::resource& resource, std::string const& mode)
{
    if (!resource.data.has_field(U("tags")))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    // TODO_ confirm the namespace with the platform (SPECIFICATION §7.1).
    resource.data[U("tags")][U("urn:x-mxl:tag:scope-mode/v1.0")] = web::json::value_of({web::json::value::string(us(mode))});
}

void tagList(nmos::resource& resource, std::map<std::string, std::vector<std::string>> const& tags)
{
    if (!resource.data.has_field(U("tags")))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    for (auto const& [name, values] : tags)
    {
        web::json::value list = web::json::value::array();
        for (auto const& item : values)
        {
            web::json::push_back(list, web::json::value::string(us(item)));
        }
        resource.data[U("tags")][us(name)] = std::move(list);
    }
}

// The output source, flow, sender and the sender's IS-05 parameters for a new output flow.
// A new flow id (format or rate change, §7) replaces the old flow; the source and the sender stay.
void applyFlow(nmos::node_model& model, NmosIds const& ids, OutputFlow const& flow, std::string const& domainId, std::string& currentFlow)
{
    using web::json::value;
    auto lock = model.write_lock();
    nmos::rational const rate{flow.rate.num, flow.rate.den};
    if (currentFlow.empty())
    {
        auto source = nmos::make_video_source(us(ids.source), us(ids.device), nmos::clock_names::clk0, rate, model.settings);
        source.data[U("label")] = value::string(U("Scopes Out Video"));
        tagGroup(source, "Scopes Out", "Video");
        nmos::insert_resource(model.node_resources, std::move(source));
    }
    else if (currentFlow != flow.flowId)
    {
        nmos::erase_resource(model.node_resources, us(currentFlow), false);
    }
    if (currentFlow != flow.flowId)
    {
        auto resource = nmos::make_coded_video_flow(us(flow.flowId), us(ids.source), us(ids.device), rate, static_cast<unsigned>(flow.width),
            static_cast<unsigned>(flow.height), nmos::interlace_modes::progressive, nmos::colorspaces::BT709, nmos::transfer_characteristics::SDR,
            sdp::samplings::YCbCr_4_2_2, 10, nmos::media_types::video_v210, model.settings);
        resource.data[U("label")] = value::string(U("Scopes Out Video"));
        resource.data[U("description")] = value::string(us(flow.description));
        tagGroup(resource, "Scopes Out", "Video");
        tagMode(resource, "luma");
        nmos::insert_resource(model.node_resources, std::move(resource));
    }
    auto const domain = us(domainId);
    if (currentFlow.empty())
    {
        auto sender = nmos::make_sender(us(ids.sender), us(flow.flowId), nmos::transports::mxl, us(ids.device), {}, {}, model.settings);
        sender.data[U("label")] = value::string(U("Scopes Out Video"));
        sender.data[U("subscription")][U("active")] = value::boolean(true);
        tagGroup(sender, "Scopes Out", "Video");
        tagMode(sender, "luma");
        nmos::insert_resource(model.node_resources, std::move(sender));
        auto connection = nmos::make_connection_mxl_sender(us(ids.sender), domain, us(flow.flowId));
        for (auto const* key : {U("active"), U("staged")})
        {
            connection.data[key][U("master_enable")] = value::boolean(true);
            connection.data[key][U("transport_params")][0][U("mxl_domain_id")] = value::string(domain);
            connection.data[key][U("transport_params")][0][U("mxl_flow_id")] = value::string(us(flow.flowId));
        }
        nmos::insert_resource(model.connection_resources, std::move(connection));
    }
    else if (currentFlow != flow.flowId)
    {
        nmos::modify_resource(model.node_resources, us(ids.sender), [&](nmos::resource& sender) {
            sender.data[U("flow_id")] = value::string(us(flow.flowId));
            sender.data[U("version")] = value::string(nmos::make_version());
        });
        nmos::modify_resource(model.connection_resources, us(ids.sender), [&](nmos::resource& connection) {
            for (auto const* key : {U("active"), U("staged")})
            {
                connection.data[key][U("transport_params")][0][U("mxl_flow_id")] = value::string(us(flow.flowId));
            }
        });
    }
    currentFlow = flow.flowId;
    model.notify();
    logInfo("nmos_output_flow", {{"flow", flow.flowId}});
}

bool uuidOk(std::string const& text)
{
    return text.size() == 36 && text[8] == '-' && text[13] == '-' && text[18] == '-' && text[23] == '-';
}
} // namespace

void NmosNode::start()
{
    if (!impl_->config.nmosEnable)
    {
        return;
    }
    impl_->thread = std::thread([this] {
        nmos::experimental::log_model logModel;
        std::ostream errorLog(std::cerr.rdbuf());
        std::filebuf discarded;
        std::ostream accessLog(&discarded);
        nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
        try
        {
            nmos::node_model nodeModel;
            impl_->model = &nodeModel;
            auto const& config = impl_->config;
            auto const& ids = impl_->ids;
            web::json::value settings = web::json::value::object();
            auto const nodeLabel = config.nmosLabel.empty() ? config.hostId : config.nmosLabel;
            settings[U("http_port")] = config.nmosPort;
            settings[U("label")] = web::json::value::string(us(nodeLabel));
            settings[U("description")] = web::json::value::string(U("mxl-scopes"));
            settings[U("seed_id")] = web::json::value::string(us(ids.node));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-scopes"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            if (!config.nmosHostAddress.empty())
            {
                settings[U("host_address")] = web::json::value::string(us(config.nmosHostAddress));
                settings[U("host_addresses")] = web::json::value::array({web::json::value::string(us(config.nmosHostAddress))});
            }
            if (!config.nmosDnsSd)
            {
                // G4: no DNS-SD browsing or advertisement unless asked for.
                settings[U("pri")] = std::numeric_limits<int>::max();
                settings[U("highest_pri")] = std::numeric_limits<int>::max();
            }
            if (!config.nmosRegistryAddress.empty())
            {
                settings[U("registry_address")] = web::json::value::string(us(config.nmosRegistryAddress));
                settings[U("registration_port")] = config.nmosRegistryPort;
                if (!config.nmosQueryAddress.empty())
                {
                    settings[U("query_address")] = web::json::value::string(us(config.nmosQueryAddress));
                }
                settings[U("query_port")] = config.nmosQueryPort;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation =
                nmos::experimental::node_implementation()
                    .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                 slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                    .on_resolve_auto([](nmos::resource const&, nmos::resource const&, web::json::value& params) {
                        if (!params.is_array() || params.size() == 0)
                        {
                            return;
                        }
                        auto& leg = params.at(0);
                        nmos::details::resolve_auto(leg, U("mxl_domain_id"), [] { return web::json::value::string(U("00000000-0000-0000-0000-000000000000")); });
                        nmos::details::resolve_auto(leg, U("mxl_flow_id"), [] { return web::json::value::null(); });
                    })
                    .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) { transportFile = web::json::value::null(); })
                    .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                        if (su(connection.id) != impl_->ids.receiver || !connection.data.has_field(U("active")))
                        {
                            return;
                        }
                        auto const& active = connection.data.at(U("active"));
                        Route next;
                        next.enable = active.has_field(U("master_enable")) && active.at(U("master_enable")).as_bool();
                        if (active.has_field(U("sender_id")) && active.at(U("sender_id")).is_string())
                        {
                            next.senderId = su(active.at(U("sender_id")).as_string());
                        }
                        if (active.has_field(U("transport_params")) && active.at(U("transport_params")).is_array() && active.at(U("transport_params")).size() > 0)
                        {
                            auto const& params = active.at(U("transport_params")).at(0);
                            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
                            {
                                next.domainId = su(params.at(U("mxl_domain_id")).as_string());
                            }
                            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
                            {
                                next.flowId = su(params.at(U("mxl_flow_id")).as_string());
                            }
                        }
                        if ((!next.domainId.empty() && !uuidOk(next.domainId)) || (!next.flowId.empty() && !uuidOk(next.flowId)))
                        {
                            throw std::runtime_error("mxl_domain_id and mxl_flow_id must be UUIDs");
                        }
                        if (next.domainId == "00000000-0000-0000-0000-000000000000")
                        {
                            next.domainId.clear();
                        }
                        if (impl_->route)
                        {
                            impl_->route(next);
                        }
                        {
                            std::lock_guard lock{impl_->pendingMu};
                            impl_->subscriptions.push_back(Impl::SubUpdate{next.senderId, next.enable && !next.senderId.empty()});
                        }
                        logInfo("nmos_activation", {{"enable", next.enable ? "true" : "false"}, {"flow", next.flowId}});
                    });

            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel, nodeLabel] {
                try
                {
                    auto lock = nodeModel.write_lock();
                    auto const& config = impl_->config;
                    auto const& ids = impl_->ids;
                    using web::json::value;
                    auto const clocks = web::json::value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
                    auto const interfaces = nmos::experimental::node_interfaces(nmos::get_host_interfaces(nodeModel.settings));
                    auto node = nmos::make_node(us(ids.node), clocks, nmos::make_node_interfaces(interfaces), nodeModel.settings);
                    node.data[U("label")] = value::string(us(nodeLabel));
                    node.data[U("description")] = value::string(U("MXL scopes"));
                    tagList(node, config.nmosTags);
                    nmos::insert_resource(nodeModel.node_resources, std::move(node));

                    auto device = nmos::make_device(us(ids.device), us(ids.node), {us(ids.sender)}, {us(ids.receiver)}, nodeModel.settings);
                    device.data[U("label")] = value::string(us(config.nmosLabel.empty() ? std::string("MXL Scopes") : config.nmosLabel + " scopes"));
                    tagList(device, config.nmosTags);
                    nmos::insert_resource(nodeModel.node_resources, std::move(device));

                    // The receiver (§6): v210 and v210a, progressive and interlaced, up to 3840×2160.
                    std::vector<nmos::rational> const rates{{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
                    auto receiver = nmos::make_receiver(us(ids.receiver), us(ids.device), nmos::transports::mxl, {}, nmos::formats::video,
                        {nmos::media_types::video_v210, nmos::media_types::video_v210a}, nodeModel.settings);
                    receiver.data[U("label")] = value::string(U("Scopes In Video"));
                    tagGroup(receiver, "Scopes In", "Video");
                    web::json::value set = web::json::value::object();
                    set[U("urn:x-nmos:cap:format:media_type")] =
                        nmos::make_caps_string_constraint({nmos::media_types::video_v210.name, nmos::media_types::video_v210a.name});
                    set[U("urn:x-nmos:cap:format:grain_rate")] = nmos::make_caps_rational_constraint(rates);
                    set[U("urn:x-nmos:cap:format:frame_width")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 1, 3840);
                    set[U("urn:x-nmos:cap:format:frame_height")] = nmos::make_caps_integer_constraint(std::vector<std::int64_t>{}, 1, 2160);
                    set[U("urn:x-nmos:cap:format:interlace_mode")] = nmos::make_caps_string_constraint(
                        {nmos::interlace_modes::progressive.name, nmos::interlace_modes::interlaced_tff.name, nmos::interlace_modes::interlaced_bff.name});
                    set[U("urn:x-nmos:cap:format:color_sampling")] = nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name});
                    set[U("urn:x-nmos:cap:format:component_depth")] = nmos::make_caps_integer_constraint({std::int64_t{10}});
                    web::json::value sets = web::json::value::array();
                    web::json::push_back(sets, std::move(set));
                    web::json::value caps = web::json::value::object();
                    caps[U("constraint_sets")] = std::move(sets);
                    caps[U("version")] = web::json::value::string(nmos::make_version());
                    receiver.data[U("caps")] = std::move(caps);
                    nmos::insert_resource(nodeModel.node_resources, std::move(receiver));
                    auto receiverConnection = nmos::make_connection_mxl_receiver(us(ids.receiver), utility::string_t{});
                    receiverConnection.data[U("active")][U("master_enable")] = value::boolean(false);
                    receiverConnection.data[U("staged")][U("master_enable")] = value::boolean(false);
                    receiverConnection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = value::null();
                    receiverConnection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = value::null();
                    nmos::insert_resource(nodeModel.connection_resources, std::move(receiverConnection));

                    nodeModel.notify();
                    lock.unlock();
                    {
                        std::lock_guard readyLock{impl_->readyMu};
                        impl_->modelReady = true;
                        impl_->readyCv.notify_all();
                    }
                    auto waitLock = nodeModel.write_lock();
                    nodeModel.wait(waitLock, [&] { return nodeModel.shutdown; });
                }
                catch (std::exception const& ex)
                {
                    std::lock_guard readyLock{impl_->readyMu};
                    impl_->error = ex.what();
                    impl_->ready = true;
                    impl_->readyCv.notify_all();
                }
            });
            // Throws when a listener cannot open (the NMOS port taken): start() then fails and
            // the process exits 75 (G6). Ready only after the listeners are open.
            nmos::server_guard guard(server);
            {
                std::unique_lock readyLock{impl_->readyMu};
                impl_->readyCv.wait(readyLock, [&] { return impl_->modelReady || !impl_->error.empty(); });
                if (!impl_->error.empty())
                {
                    throw std::runtime_error(impl_->error);
                }
                impl_->ready = true;
                impl_->running.store(true);
                impl_->readyCv.notify_all();
            }
            logInfo("nmos_node_ready", {{"port", std::to_string(impl_->config.nmosPort)}, {"node", impl_->ids.node}});
            std::string currentFlow;
            while (impl_->running.load())
            {
                std::vector<Impl::SubUpdate> batch;
                std::optional<std::pair<OutputFlow, std::string>> flow;
                {
                    std::lock_guard lock{impl_->pendingMu};
                    batch.swap(impl_->subscriptions);
                    flow.swap(impl_->flow);
                }
                if (flow)
                {
                    applyFlow(nodeModel, impl_->ids, flow->first, flow->second, currentFlow);
                }
                if (!batch.empty())
                {
                    auto lock = nodeModel.write_lock();
                    for (auto const& update : batch)
                    {
                        nmos::modify_resource(nodeModel.node_resources, us(impl_->ids.receiver), [&](nmos::resource& resource) {
                            resource.data[U("subscription")][U("sender_id")] =
                                update.sender.empty() ? web::json::value::null() : web::json::value::string(us(update.sender));
                            resource.data[U("subscription")][U("active")] = web::json::value::boolean(update.active);
                            resource.data[U("version")] = web::json::value::string(nmos::make_version());
                        });
                    }
                    nodeModel.notify();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                // G8: the IS-04 resources go away (node last), so nmos-cpp sends the DELETEs.
                auto lock = nodeModel.write_lock();
                std::vector<nmos::id> resourceIds;
                nmos::id nodeId;
                for (auto const& resource : nodeModel.node_resources)
                {
                    if (resource.type == nmos::types::node)
                    {
                        nodeId = resource.id;
                    }
                    else if (resource.type == nmos::types::device || resource.type == nmos::types::source || resource.type == nmos::types::flow ||
                             resource.type == nmos::types::sender || resource.type == nmos::types::receiver)
                    {
                        resourceIds.push_back(resource.id);
                    }
                }
                for (auto const& id : resourceIds)
                {
                    nmos::erase_resource(nodeModel.node_resources, id, false);
                }
                if (!nodeId.empty())
                {
                    nmos::erase_resource(nodeModel.node_resources, nodeId, false);
                }
                nodeModel.notify();
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                auto const url = "http://" + impl_->config.nmosQueryAddress + ":" + std::to_string(impl_->config.nmosQueryPort) + "/x-nmos/query/v1.3/nodes/" +
                                 impl_->ids.node;
                auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
                while (std::chrono::steady_clock::now() < deadline && httpGet(url, 500).status != 404)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
        }
        catch (std::exception const& ex)
        {
            std::lock_guard readyLock{impl_->readyMu};
            if (impl_->error.empty())
            {
                impl_->error = ex.what();
            }
            impl_->ready = true;
            impl_->readyCv.notify_all();
            logError("nmos_node_failed", {{"error", ex.what()}});
        }
        impl_->model = nullptr;
    });

    std::unique_lock readyLock{impl_->readyMu};
    impl_->readyCv.wait_for(readyLock, std::chrono::seconds(20), [&] { return impl_->ready; });
    if (!impl_->error.empty() || !impl_->ready)
    {
        auto const message = impl_->error.empty() ? std::string("NMOS node did not become ready") : impl_->error;
        readyLock.unlock();
        stop();
        throw std::runtime_error(message);
    }
    readyLock.unlock();
    // The listeners may open even where another process holds the port (address reuse): only
    // our own Node API answering with our node id proves the port is ours (G6: else exit 75).
    auto const self = "http://127.0.0.1:" + std::to_string(impl_->config.nmosPort) + "/x-nmos/node/v1.3/self";
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        auto const answer = httpGet(self, 500);
        if (answer.status == 200 && answer.body.find(impl_->ids.node) != std::string::npos)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    stop();
    throw std::runtime_error("the NMOS Node API on port " + std::to_string(impl_->config.nmosPort) + " does not answer as this node (port taken?)");
}

void NmosNode::stop()
{
    impl_->running.store(false);
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}
#endif
} // namespace scopes

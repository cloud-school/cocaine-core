#include <boost/algorithm/string/join.hpp>
#include <boost/assign.hpp>

#include "cocaine/drivers/zeromq_server.hpp"
#include "cocaine/engine.hpp"

using namespace cocaine::engine::driver;
using namespace cocaine::networking;

zeromq_server_job_t::zeromq_server_job_t(zeromq_server_t* driver, const route_t& route):
    job_t(driver, job::policy_t()),
    m_route(route)
{ }

void zeromq_server_job_t::react(const events::chunk_t& event) {
    send(event.message);
}

void zeromq_server_job_t::react(const events::error_t& event) {
    Json::Value object(Json::objectValue);
    
    object["code"] = event.code;
    object["message"] = event.message;

    std::string response(Json::FastWriter().write(object));
    zmq::message_t message(response.size());
    memcpy(message.data(), response.data(), response.size());

    send(message);
}

bool zeromq_server_job_t::send(zmq::message_t& chunk) {
    zmq::message_t message;
    zeromq_server_t* server = static_cast<zeromq_server_t*>(m_driver);
    
    // Send the identity
    for(route_t::const_iterator id = m_route.begin(); id != m_route.end(); ++id) {
        message.rebuild(id->size());
        memcpy(message.data(), id->data(), id->size());
        
        if(!server->socket().send(message, ZMQ_SNDMORE)) {
            return false;
        }
    }

    // Send the delimiter
    message.rebuild(0);

    if(!server->socket().send(message, ZMQ_SNDMORE)) {
        return false;
    }

    // Send the chunk
    return server->socket().send(chunk);
}

zeromq_server_t::zeromq_server_t(engine_t* engine, const std::string& method, const Json::Value& args) try:
    driver_t(engine, method),
    m_socket(m_engine->context(), ZMQ_ROUTER, boost::algorithm::join(
        boost::assign::list_of
            (config_t::get().core.instance)
            (config_t::get().core.hostname)
            (m_engine->name())
            (method),
        "/")
    )
{
    std::string endpoint(args.get("endpoint", "").asString());

    if(endpoint.empty()) {
        throw std::runtime_error("no endpoint has been specified for the '" + m_method + "' task");
    }
    
    m_socket.bind(endpoint);

    m_watcher.set<zeromq_server_t, &zeromq_server_t::event>(this);
    m_watcher.start(m_socket.fd(), ev::READ);
    m_processor.set<zeromq_server_t, &zeromq_server_t::process>(this);
    m_processor.start();
} catch(const zmq::error_t& e) {
    throw std::runtime_error("network failure in '" + m_method + "' task - " + e.what());
}

zeromq_server_t::~zeromq_server_t() {
    m_watcher.stop();
    m_processor.stop();
}

Json::Value zeromq_server_t::info() const {
    Json::Value result(Json::objectValue);

    result["statistics"] = stats();
    result["type"] = "zeromq-server";
    result["endpoint"] = m_socket.endpoint();
    result["route"] = m_socket.route();

    return result;
}

void zeromq_server_t::event(ev::io&, int) {
    if(m_socket.pending()) {
        m_watcher.stop();
        m_processor.start();
    }
}

void zeromq_server_t::process(ev::idle&, int) {
    if(m_socket.pending()) {
        zmq::message_t message;
        unsigned int route_part_counter = MAX_ROUTE_PARTS;
        route_t route;

        while(route_part_counter--) {
            m_socket.recv(&message);

            if(!message.size()) {
                break;
            }

            route.push_back(std::string(
                static_cast<const char*>(message.data()),
                message.size()));
        }

        if(route.empty() || !route_part_counter) {
            syslog(LOG_ERR, "driver [%s:%s]: got a corrupted request - no route", 
                m_engine->name().c_str(), m_method.c_str());
            return;
        }

        while(m_socket.more()) {
            boost::shared_ptr<zeromq_server_job_t> job(new zeromq_server_job_t(this, route));

            m_socket.recv(job->request());
            m_engine->enqueue(job);
        }
    } else {
        m_watcher.start(m_socket.fd(), ev::READ);
        m_processor.stop();
    }
}

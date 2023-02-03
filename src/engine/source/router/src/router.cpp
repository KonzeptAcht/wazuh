#include <router/router.hpp>

#include <fstream>
#include <iostream>

#include <builder.hpp>

#include <parseEvent.hpp>

namespace router
{
constexpr auto WAIT_DEQUEUE_TIMEOUT_USEC = 1 * 1000000;

std::optional<base::Error> Router::addRoute(const std::string& name, const std::string& envName, int priority)
{
    try
    {
        // Build the same route for each thread
        std::vector<Route> routeInstances {};
        routeInstances.reserve(m_numThreads);
        for (std::size_t i = 0; i < m_numThreads; ++i)
        {
            // routeInstances[i] = builder::Route {jsonDefinition, m_registry};
            auto filter = m_builder->buildRoute(name);

            routeInstances.emplace_back(Route {filter, envName, priority});
        }

        // Add the environment
        auto err = m_environmentManager->addEnvironment(envName);
        if (err)
        {
            return base::Error {err.value()};
        }

        // Link the route to the environment
        {
            std::unique_lock lock {m_mutexRoutes};
            std::optional<base::Error> err = std::nullopt;
            // Check if the route already exists, should we update it?
            if (m_namePriority.find(name) != m_namePriority.end())
            {
                err = base::Error {fmt::format("Route '{}' already exists", name)};
            }
            // Check if the priority is already taken
            if (m_priorityRoute.find(priority) != m_priorityRoute.end())
            {
                err = base::Error {fmt::format("Priority '{}' already taken", priority)};
            }
            // check error
            if (err)
            {
                lock.unlock();
                m_environmentManager->deleteEnvironment(envName);
                return err;
            }
            m_namePriority.insert(std::make_pair(name, priority));
            m_priorityRoute.insert(std::make_pair(priority, std::move(routeInstances)));
        }
    }
    catch (const std::exception& e)
    {
        return base::Error {e.what()};
    }
    dumpTableToStorage();
    return std::nullopt;
}

std::optional<base::Error> Router::removeRoute(const std::string& routeName)
{
    std::unique_lock lock {m_mutexRoutes};

    auto it = m_namePriority.find(routeName);
    if (it == m_namePriority.end())
    {
        return base::Error {fmt::format("Route '{}' not found", routeName)};
    }
    const auto priority = it->second;

    auto it2 = m_priorityRoute.find(priority);
    if (it2 == m_priorityRoute.end())
    {
        return base::Error {fmt::format("Priority '{}' not found", priority)}; // Should never happen
    }
    const auto envName = it2->second.front().getTarget();
    // Remove from maps
    m_namePriority.erase(it);
    m_priorityRoute.erase(it2);
    lock.unlock();

    dumpTableToStorage();
    return m_environmentManager->deleteEnvironment(envName);
}

std::vector<std::tuple<std::string, std::size_t, std::string>> Router::getRouteTable()
{
    std::shared_lock lock {m_mutexRoutes};
    std::vector<std::tuple<std::string, std::size_t, std::string>> table {};
    table.reserve(m_namePriority.size());
    try
    {
        for (const auto& route : m_namePriority)
        {
            const auto& name = route.first;
            const auto& priority = route.second;
            const auto& envName = m_priorityRoute.at(priority).front().getTarget();
            table.emplace_back(name, priority, envName);
        }
    }
    catch (const std::exception& e)
    {
        WAZUH_LOG_ERROR("Error getting route table: {}", e.what()); // Should never happen
    }
    lock.unlock();

    // Sort by priority
    std::sort(table.begin(), table.end(), [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });

    return table;
}

std::optional<base::Error> Router::changeRoutePriority(const std::string& name, int priority)
{
    std::unique_lock lock {m_mutexRoutes};

    auto it = m_namePriority.find(name);
    if (it == m_namePriority.end())
    {
        return base::Error {fmt::format("Route '{}' not found", name)};
    }
    const auto oldPriority = it->second;

    if (oldPriority == priority)
    {
        return std::nullopt;
    }

    auto it2 = m_priorityRoute.find(oldPriority);
    if (it2 == m_priorityRoute.end())
    {
        return base::Error {fmt::format("Priority '{}' not found", oldPriority)}; // Should never happen
    }

    // Check if the priority is already taken
    if (m_priorityRoute.find(priority) != m_priorityRoute.end())
    {
        return base::Error {fmt::format("Priority '{}' already taken", priority)};
    }

    // update the route priority
    try
    {
        for (auto& route : it2->second)
        {
            route.setPriority(priority);
        }
    }
    catch (const std::exception& e)
    {
        return base::Error {e.what()};
    }

    // Update maps
    it->second = priority;
    m_priorityRoute.insert(std::make_pair(priority, std::move(it2->second)));
    m_priorityRoute.erase(it2);
    lock.unlock();

    dumpTableToStorage();
    return std::nullopt;
}

std::optional<base::Error> Router::enqueueEvent(base::Event event)
{
    if (!m_isRunning.load() || !m_queue)
    {
        return base::Error {"The router queue is not initialized"};
    }
    if (m_queue->try_enqueue(std::move(event)))
    {
        return std::nullopt;
    }
    return base::Error {"The router queue is in high load"};
}

std::optional<base::Error> Router::run(std::shared_ptr<concurrentQueue> queue)
{
    std::shared_lock lock {m_mutexRoutes};

    if (m_isRunning.load())
    {
        return base::Error {"The router is already running"};
    }
    m_queue = queue; // Update queue
    m_isRunning.store(true);

    for (std::size_t i = 0; i < m_numThreads; ++i)
    {
        m_threads.emplace_back(
            [this, queue, i]()
            {
                while (m_isRunning.load())
                {
                    base::Event event {};
                    if (queue->wait_dequeue_timed(event, WAIT_DEQUEUE_TIMEOUT_USEC))
                    {
                        std::shared_lock lock {m_mutexRoutes};
                        for (auto& route : m_priorityRoute)
                        {
                            if (route.second[i].accept(event))
                            {
                                const auto& target = route.second[i].getTarget();
                                lock.unlock();
                                m_environmentManager->forwardEvent(target, i, std::move(event));
                                break;
                            }
                        }
                    }
                }
                WAZUH_LOG_DEBUG("Thread [{}] router finished.", i);
            });
    };

    return std::nullopt;
}

void Router::stop()
{
    if (!m_isRunning.load())
    {
        return;
    }
    m_isRunning.store(false);
    for (auto& thread : m_threads)
    {
        thread.join();
    }
    m_threads.clear();

    WAZUH_LOG_DEBUG("Router stopped.");
}

/********************************************************************
 *                  callback API Request
 ********************************************************************/

api::CommandFn Router::apiCallbacks()
{
    return [this](const json::Json params)
    {
        api::WazuhResponse response {};
        const auto action = params.getString("/action");

        if (!action)
        {
            response.message(R"(Missing "action" parameter)");
        }
        else if (action.value() == "set")
        {
            response = apiSetRoute(params);
        }
        else if (action.value() == "get")
        {
            response = apiGetRoutes(params);
        }
        else if (action.value() == "delete")
        {
            response = apiDeleteRoute(params);
        }
        else if (action.value() == "change_priority")
        {
            response = apiChangeRoutePriority(params);
        }
        else if(action.value() == "enqueue_event")
        {
            response = apiEnqueueEvent(params);
        }
        else
        {
            response.message(fmt::format("Invalid action '{}'", action.value()));
        }
        return response;
    };
}

/********************************************************************
 *                  private callback API Request
 ********************************************************************/

api::WazuhResponse Router::apiSetRoute(const json::Json& params)
{
    api::WazuhResponse response {};
    const auto name = params.getString(JSON_PATH_NAME);
    const auto priority = params.getInt(JSON_PATH_PRIORITY);
    const auto target = params.getString(JSON_PATH_TARGET);
    if (!name)
    {
        response.message(R"(Error: Error: Missing "name" parameter)");
    } else if (!priority)
    {
        response.message(R"(Error: Error: Missing "priority" parameter)");
    } else if (!target)
    {
        response.message(R"(Error: Error: Missing "target" parameter)");
    }
    else
    {

        const auto err = addRoute(name.value(), target.value(), priority.value());
        if (err)
        {
            response.message(std::string {"Error: "} + err.value().message);
        }
        else
        {
            response.message(fmt::format("Route '{}' added", name.value()));
        }
    }
    return response;
}

api::WazuhResponse Router::apiGetRoutes(const json::Json& params)
{
    auto data = tableToJson();
    return api::WazuhResponse {data, "Ok"};
}

api::WazuhResponse Router::apiDeleteRoute(const json::Json& params)
{
    api::WazuhResponse response {};
    const auto name = params.getString(JSON_PATH_NAME);
    if (!name)
    {
        response.message(R"(Error: Error: Missing "priority" parameter)");
    }
    else
    {
        const auto err = removeRoute(name.value());
        if (err)
        {
            response.message(std::string {"Error: "} + err.value().message);
        }
        else
        {
            response.message(fmt::format("Route '{}' deleted", name.value()));
        }
    }
    return response;
}

api::WazuhResponse Router::apiChangeRoutePriority(const json::Json& params)
{
    api::WazuhResponse response {};
    const auto name = params.getString(JSON_PATH_NAME);
    const auto priority = params.getInt(JSON_PATH_PRIORITY);

    if (!name)
    {
        response.message(R"(Error: Error: Missing "priority" parameter)");
    }
    else if (!priority)
    {
        response.message(R"(Missing "priority" parameter)");
    }
    else
    {
        const auto err = changeRoutePriority(name.value(), priority.value());
        if (err)
        {
            response.message(err.value().message);
        }
        else
        {
            response.message(fmt::format("Route '{}' priority changed to '{}'", name.value(), priority.value()));
        }
    }

    return response;
}

api::WazuhResponse Router::apiEnqueueEvent(const json::Json& params)
{
    api::WazuhResponse response {};
    const auto event = params.getString(JSON_PATH_EVENT);
    if (!event)
    {
        response.message(R"(Error: Missing "event" parameter)");
    }
    else
    {
        try {
            base::Event ev = base::parseEvent::parseOssecEvent(event.value());
            auto err = enqueueEvent(ev);
            if (err)
            {
                response.message(err.value().message);
            }
            else
            {
                response.message("Ok");
            }
        } catch (const std::exception& e) {
            response.message(fmt::format("Error: {} ", e.what()));
        }
    }
    return response;
}

json::Json Router::tableToJson()
{
    json::Json data {};
    data.setArray();

    const auto table = getRouteTable();
    for (const auto& [name, priority, envName] : table)
    {
        json::Json entry {};
        entry.setString(name, JSON_PATH_NAME);
        entry.setInt(priority, JSON_PATH_PRIORITY);
        entry.setString(envName, JSON_PATH_TARGET);
        data.appendJson(entry);
    }
    return data;
}

void Router::dumpTableToStorage()
{
    const auto err = m_store->update(ROUTES_TABLE_NAME, tableToJson());
    if (err)
    {
        WAZUH_LOG_ERROR("Error updating routes table: {}", err.value().message);
        exit(10);
        // TODO: throw exception and exit program (Review when the exit policy is implemented)
    }
}

} // namespace router
#include "api/test/sessionManager.hpp"

#include <logging/logging.hpp>

namespace
{

using std::string;

}

namespace api::sessionManager
{

SessionManager& SessionManager::getInstance(void)
{
    static std::once_flag flag;
    static std::unique_ptr<SessionManager> instance;

    // Thread-safe initialization.
    std::call_once(flag, []() { instance = std::make_unique<SessionManager>(); });

    return *instance;
}

std::optional<base::Error> SessionManager::createSession(const string& sessionName,
                                                         const string& policyName,
                                                         const string& filterName,
                                                         const string& routeName,
                                                         uint32_t lifespan,
                                                         const string& description)
{
    std::unique_lock<std::shared_mutex> lock(m_sessionMutex);

    if (m_activeSessions.count(sessionName) > 0)
    {
        return base::Error {fmt::format("Session name '{}' already exists", sessionName)};
    }

    if (m_policyMap.count(policyName) > 0)
    {
        return base::Error {
            fmt::format("Policy '{}' is already assigned to a route ('')", policyName, m_policyMap[policyName])};
    }

    Session session(sessionName, policyName, filterName, routeName, lifespan, description);
    m_activeSessions.emplace(sessionName, session);
    m_routeMap.emplace(routeName, sessionName);
    m_policyMap.emplace(policyName, routeName);

    LOG_DEBUG("Session created: ID={}, Name={}, Creation Date={}, Policy Name={}, Route Name={}, Life Span={}, "
              "Description=\n",
              session.getSessionID(),
              session.getSessionName(),
              session.getCreationDate(),
              session.getPolicyName(),
              session.getRouteName(),
              session.getLifespan(),
              session.getDescription());

    return std::nullopt;
}

std::vector<string> SessionManager::getSessionsList(void)
{
    std::shared_lock<std::shared_mutex> lock(m_sessionMutex);

    std::vector<string> sessionNames;

    for (const auto& pair : m_activeSessions)
    {
        sessionNames.push_back(pair.first);
    }
    return sessionNames;
}

std::optional<Session> SessionManager::getSession(const string& sessionName)
{
    std::shared_lock<std::shared_mutex> lock(m_sessionMutex);

    auto it = m_activeSessions.find(sessionName);
    if (it != m_activeSessions.end())
    {
        return it->second;
    }
    return std::nullopt;
}

bool SessionManager::deleteSessions(const bool removeAll, const string sessionName)
{
    std::unique_lock<std::shared_mutex> lock(m_sessionMutex);

    bool sessionRemoved {false};

    if (removeAll)
    {
        m_activeSessions.clear();
        m_policyMap.clear();
        m_routeMap.clear();

        sessionRemoved = true;
    }
    else
    {
        // Remove a specific session by sessionName
        auto sessionIt = m_activeSessions.find(sessionName);
        if (sessionIt != m_activeSessions.end())
        {
            const auto& policyName = sessionIt->second.getPolicyName();
            const auto& routeName = sessionIt->second.getRouteName();

            m_activeSessions.erase(sessionIt);
            m_policyMap.erase(policyName);
            m_routeMap.erase(routeName);

            sessionRemoved = true;
        }
    }

    return sessionRemoved;
}

bool SessionManager::deleteSession(const string& sessionName)
{
    return deleteSessions(false, sessionName);
}

bool SessionManager::doesSessionExist(const std::string& sessionName)
{
    std::shared_lock<std::shared_mutex> lock(m_sessionMutex);

    bool doesExist {false};

    if (m_activeSessions.count(sessionName) > 0)
    {
        doesExist = true;
    }

    return doesExist;
}

} // namespace api::sessionManager

/*
 * Wazuh logging helper
 * Copyright (C) 2015, Wazuh Inc.
 * September 15, 2022.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#ifndef LOGGER_HELPER_H
#define LOGGER_HELPER_H

#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include "commonDefs.h"

// We can't use std::source_location until C++20
#define LogEndl Log::SourceFile {__FILE__, __LINE__, __func__}
#define logInfo(X, Y) Log::Logger::info(X, Y, LogEndl)
#define logWarn(X, Y) Log::Logger::warning(X, Y, LogEndl)
#define logDebug1(X, Y) Log::Logger::debug(X, Y, LogEndl)
#define logDebug2(X, Y) Log::Logger::debugVerbose(X, Y, LogEndl)
#define logError(X, Y) Log::Logger::error(X, Y, LogEndl)

#define VS_WM_NAME         "vulnerability-scanner"
#define WM_VULNSCAN_LOGTAG "wazuh-modulesd:" VS_WM_NAME

namespace Log
{
    enum LOG_LEVEL
    {
        INFO = 0,
        WARNING = 1,
        DEBUG = 2,
        DEBUG_VERBOSE = 3,
        ERROR = 4
    };

    struct SourceFile
    {
        const char* file;
        int line;
        const char* func;
    };

    static std::unordered_map<LOG_LEVEL, full_log_fnc_t> m_logFunctions = {};

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wunused-function"

    static void assignLogFunction(full_log_fnc_t infoLogFunction, full_log_fnc_t warningLogFunction, full_log_fnc_t debugLogFunction, full_log_fnc_t debugVerboseLogFunction, full_log_fnc_t errorLogFunction)
    {
        if (infoLogFunction)
        {
            m_logFunctions.emplace(LOG_LEVEL::INFO, infoLogFunction);
        }

        if (warningLogFunction)
        {
            m_logFunctions.emplace(LOG_LEVEL::WARNING, warningLogFunction);
        }

        if (debugLogFunction)
        {
            m_logFunctions.emplace(LOG_LEVEL::DEBUG, debugLogFunction);
        }

        if (debugVerboseLogFunction)
        {
            m_logFunctions.emplace(LOG_LEVEL::DEBUG_VERBOSE, debugVerboseLogFunction);;
        }

        if (errorLogFunction)
        {
            m_logFunctions.emplace(LOG_LEVEL::ERROR, errorLogFunction);
        }
    }

    #pragma GCC diagnostic pop

    /**
     * @brief Logging helper class.
     *
     */
    class Logger final
    {
        public:
            /**
             * @brief INFO log.
             *
             * @param tag Module tag.
             * @param msg Message to be logged.
             * @param sourceFile Log location.
             */
            static void info(const std::string& tag, const std::string& msg, SourceFile sourceFile)
            {
                try
                {
                    auto info = m_logFunctions.at(LOG_LEVEL::INFO);
                    if (info) {
                        info(tag.c_str(), sourceFile.file, sourceFile.line, sourceFile.func, msg.c_str());
                    }
                }
                catch (...) {}
            }

            /**
             * @brief WARNING LOG.
             *
             * @param tag Module tag.
             * @param msg Message to be logged.
             * @param sourceFile Log location.
             */
            static void warning(const std::string& tag, const std::string& msg, SourceFile sourceFile)
            {
                try {
                    auto warning = m_logFunctions.at(LOG_LEVEL::WARNING);
                    if (warning) {
                        warning(tag.c_str(), sourceFile.file, sourceFile.line, sourceFile.func, msg.c_str());
                    }
                }
                catch (...) {}
            }

            /**
             * @brief DEBUG log.
             *
             * @param tag Module tag.
             * @param msg Message to be logged.
             * @param sourceFile Log location.
             */
            static void debug(const std::string& tag, const std::string& msg, SourceFile sourceFile)
            {
                try {
                    auto debug = m_logFunctions.at(LOG_LEVEL::DEBUG);
                    if (debug) {
                        m_logFunctions.at(LOG_LEVEL::DEBUG)(tag.c_str(), sourceFile.file, sourceFile.line, sourceFile.func, msg.c_str());
                    }
                }
                catch (...) {}
            }

            /**
             * @brief DEBUG VERBOSE log.
             *
             * @param tag Module tag.
             * @param msg Message to be logged.
             * @param sourceFile Log location.
             */
            static void debugVerbose(const std::string& tag, const std::string& msg, SourceFile sourceFile)
            {
                try {
                    auto debugVerbose = m_logFunctions.at(LOG_LEVEL::DEBUG_VERBOSE);
                    if (debugVerbose) {
                        debugVerbose(tag.c_str(), sourceFile.file, sourceFile.line, sourceFile.func, msg.c_str());
                    }
                }
                catch (...) {}
            }

            /**
             * @brief ERROR log.
             *
             * @param tag Module tag.
             * @param msg Message to be logged.
             * @param sourceFile Log location.
             */
            static void error(const std::string& tag, const std::string& msg, SourceFile sourceFile)
            {
                try {
                    auto error = m_logFunctions.at(LOG_LEVEL::ERROR);
                    if (error) {
                        error(tag.c_str(), sourceFile.file, sourceFile.line, sourceFile.func, msg.c_str());
                    }
                }
                catch (...) {}
            }
    };
} // namespace Log
#endif // LOGGER_HELPER_H
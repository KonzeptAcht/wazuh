/* Copyright (C) 2015-2022, Wazuh Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */
#include "opBuilderHelperMap.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <re2/re2.h>

#include "syntax.hpp"
#include <baseHelper.hpp>
#include <utils/ipUtils.hpp>
#include <utils/stringUtils.hpp>

namespace
{

/**
 * @brief Operators supported by the string helpers.
 *
 */
enum class StringOperator
{
    UP,
    LO,
    TR
};

/**
 * @brief Operators supported by the int helpers.
 *
 */
enum class IntOperator
{
    SUM,
    SUB,
    MUL,
    DIV
};

IntOperator strToOp(const helper::base::Parameter& op)
{
    if ("sum" == op.m_value)
    {
        return IntOperator::SUM;
    }
    else if ("sub" == op.m_value)
    {
        return IntOperator::SUB;
    }
    else if ("mul" == op.m_value)
    {
        return IntOperator::MUL;
    }
    else if ("div" == op.m_value)
    {
        return IntOperator::DIV;
    }
    throw std::runtime_error(fmt::format("[builders::strToOp()] operation not support"));
}

/**
 * @brief Tranform the string in `field` path in the base::Event `e` according to the
 * `op` definition and the `value` or the `refValue`
 *
 * @param definition The transformation definition. i.e : field: +s_[up|lo]/value|$ref
 * @param op The operator to use:
 * - `UP`: Upper case
 * - `LO`: Lower case
 * @return base::Expression
 */
base::Expression opBuilderHelperStringTransformation(const std::any& definition,
                                                     StringOperator op)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 1);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Depending on rValue type we store the reference or the string value, string in both
    // cases
    std::string rValue {};
    const helper::base::Parameter rightParameter {parameters[0]};
    const auto rValueType {rightParameter.m_type};
    rValue = rightParameter.m_value;

    // Depending on the operator we return the correct function
    std::function<std::string(const std::string& value)> transformFunction;
    switch (op)
    {
        case StringOperator::UP:
            transformFunction = [](const std::string& value)
            {
                std::string result;
                std::transform(
                    value.begin(), value.end(), std::back_inserter(result), ::toupper);
                return result;
            };
            break;
        case StringOperator::LO:
            transformFunction = [](const std::string& value)
            {
                std::string result;
                std::transform(
                    value.begin(), value.end(), std::back_inserter(result), ::tolower);
                return result;
            };
            break;
        default: break;
    }

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: [{}] not found", name, rightParameter.m_value)};
    const auto failureTrace2 {fmt::format("[{}] -> Failure", name)};

    // Function that implements the helper
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // We assert that references exists, checking if the optional from Json getter
            // is empty ot not. Then if is a reference we get the value from the event,
            // otherwise we get the value from the parameter

            // REF

            if (helper::base::Parameter::Type::REFERENCE == rValueType)
            {
                const auto resolvedRValue {event->getString(rValue)};
                if (!resolvedRValue.has_value())
                {
                    return base::result::makeFailure(event, failureTrace1);
                }

                else
                {
                    auto res {transformFunction(resolvedRValue.value())};
                    event->setString(res, targetField);
                    return base::result::makeSuccess(event, successTrace);
                }
            }
            else
            {
                const auto res {transformFunction(rValue)};
                event->setString(res, targetField);
                return base::result::makeSuccess(event, successTrace);
            }
        });
}

/**
 * @brief Tranform the int in `field` path in the base::Event `e` according to the
 * `op` definition and the `value` or the `refValue`
 *
 * @param definition The transformation definition. i.e : +i_calc/[+|-|*|/]/val|$ref/
 * @param op The operator to use:
 * - `SUM`: Sum
 * - `SUB`: Subtract
 * - `MUL`: Multiply
 * - `DIV`: Divide
 * @return base::Expression
 */
base::Expression
opBuilderHelperIntTransformation(const std::string& targetField,
                                 IntOperator op,
                                 const helper::base::Parameter& rightParameter,
                                 const std::string& name)
{
    // Depending on rValue type we store the reference or the integer value
    std::variant<std::string, int> rValue {};
    auto rValueType {rightParameter.m_type};
    switch (rightParameter.m_type)
    {
        case helper::base::Parameter::Type::VALUE:
            try
            {
                rValue = std::stoi(rightParameter.m_value);
            }
            catch (const std::exception& e)
            {
                std::throw_with_nested(std::runtime_error(
                    fmt::format("[builders::opBuilderHelperIntTransformation()] could "
                                "not convert {} to int",
                                rightParameter.m_value)));
            }
            if (IntOperator::DIV == op && 0 == std::get<int>(rValue))
            {
                throw std::runtime_error(fmt::format(
                    "[builders::opBuilderHelperIntTransformation()] division by zero"));
            }

            break;

        case helper::base::Parameter::Type::REFERENCE:
            rValue = rightParameter.m_value;
            break;

        default:
            throw std::runtime_error(
                fmt::format("[builders::opBuilderHelperIntTransformation()] invalid "
                            "parameter type for {}",
                            rightParameter.m_value));
    }

    // Depending on the operator we return the correct function
    std::function<int(int l, int r)> transformFunction;
    switch (op)
    {
        case IntOperator::SUM:
            transformFunction = [](int l, int r)
            {
                return l + r;
            };
            break;
        case IntOperator::SUB:
            transformFunction = [](int l, int r)
            {
                return l - r;
            };
            break;
        case IntOperator::MUL:
            transformFunction = [](int l, int r)
            {
                return l * r;
            };
            break;
        case IntOperator::DIV:
            transformFunction = [](int l, int r)
            {
                if (0 == r)
                {
                    throw std::runtime_error(
                        fmt::format("[builders::opBuilderHelperIntTransformation()] "
                                    "division by zero"));
                }

                return l / r;
            };
            break;
        default: break;
    }

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: [{}] not found", name, targetField)};
    const auto failureTrace2 {
        fmt::format("[{}] -> Failure: [{}] not found", name, rightParameter.m_value)};
    const auto failureTrace3 {fmt::format("[{}] -> Failure", name)};
    const auto failureTrace4 = fmt::format(
        "[{}] -> Failure: [{}] division by zero", name, rightParameter.m_value);

    // Function that implements the helper
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // We assert that references exists, checking if the optional from Json getter
            // is empty ot not. Then if is a reference we get the value from the event,
            // otherwise we get the value from the parameter

            const auto lValue {event->getInt(targetField)};
            if (!lValue.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            if (helper::base::Parameter::Type::REFERENCE == rValueType)
            {
                const auto resolvedRValue {event->getInt(std::get<std::string>(rValue))};
                if (!resolvedRValue.has_value())
                {
                    return base::result::makeFailure(event, failureTrace2);
                }
                else
                {
                    if (IntOperator::DIV == op && 0 == resolvedRValue)
                    {
                        return base::result::makeFailure(event, failureTrace4);
                    }
                    auto res {transformFunction(lValue.value(), resolvedRValue.value())};
                    event->setInt(res, targetField);
                    return base::result::makeSuccess(event, successTrace);
                }
            }
            else
            {
                const auto res {transformFunction(lValue.value(), std::get<int>(rValue))};
                event->setInt(res, targetField);
                return base::result::makeSuccess(event, successTrace);
            }
        });
}

std::optional<std::string> hashStringSHA1(std::string& input)
{
    // Sha1 digest len (20) * 2 (hex chars per byte)
    constexpr int OS_SHA1_HEXDIGEST_SIZE = (SHA_DIGEST_LENGTH * 2);
    constexpr int OS_SHA1_ARRAY_SIZE_LEN = OS_SHA1_HEXDIGEST_SIZE + 1;

    char* parameter = NULL;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size;

    EVP_MD_CTX* ctx = EVP_MD_CTX_create();
    if (!ctx)
    {
        // Failed during hash context creation
        return std::nullopt;
    }

    if (1 != EVP_DigestInit_ex(ctx, EVP_sha1(), NULL))
    {
        // Failed during hash context initialization
        EVP_MD_CTX_destroy(ctx);
        return std::nullopt;
    }

    if (1 != EVP_DigestUpdate(ctx, input.c_str(), input.length()))
    {
        // Failed during hash context update
        return std::nullopt;
    }

    EVP_DigestFinal_ex(ctx, digest, &digest_size);
    EVP_MD_CTX_destroy(ctx);

    // OS_SHA1_Hexdigest(digest, hexdigest);
    char output[OS_SHA1_ARRAY_SIZE_LEN];
    for (size_t n = 0; n < SHA_DIGEST_LENGTH; n++)
    {
        sprintf(&output[n * 2], "%02x", digest[n]);
    }

    return {output};
}

} // namespace

namespace builder::internals::builders
{

using builder::internals::syntax::REFERENCE_ANCHOR;
//*************************************************
//*           String tranform                     *
//*************************************************

// field: +s_up/value|$ref
base::Expression opBuilderHelperStringUP(const std::any& definition)
{
    auto expression {opBuilderHelperStringTransformation(definition, StringOperator::UP)};
    return expression;
}

// field: +s_lo/value|$ref
base::Expression opBuilderHelperStringLO(const std::any& definition)
{
    auto expression {opBuilderHelperStringTransformation(definition, StringOperator::LO)};
    return expression;
}

// field: +s_trim/[begin | end | both]/char
base::Expression opBuilderHelperStringTrim(const std::any& definition)
{

    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 2);
    // Parameter type check
    helper::base::checkParameterType(parameters[0], helper::base::Parameter::Type::VALUE);
    helper::base::checkParameterType(parameters[1], helper::base::Parameter::Type::VALUE);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Get trim type
    char trimType = parameters[0].m_value == "begin"  ? 's'
                    : parameters[0].m_value == "end"  ? 'e'
                    : parameters[0].m_value == "both" ? 'b'
                                                      : '\0';
    if ('\0' == trimType)
    {
        throw std::runtime_error("Invalid trim type for s_trim operator");
    }

    // get trim char
    std::string trimChar {parameters[1].m_value};
    if (trimChar.size() != 1)
    {
        throw std::runtime_error("Invalid trim char for s_trim operator");
    }

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: [{}] not found", name, targetField)};
    const auto failureTrace2 {
        fmt::format("[{}] -> Failure: [{}] not found", name, parameters[1].m_value)};
    const auto failureTrace3 {fmt::format("[{}] -> Failure", name)};

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // Get field value
            auto resolvedField {event->getString(targetField)};

            // Check if field is a string
            if (!resolvedField.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            // Get string
            std::string strToTrim {resolvedField.value()};

            // Trim
            switch (trimType)
            {
                case 's':
                    // Trim begin
                    strToTrim.erase(0, strToTrim.find_first_not_of(trimChar));
                    break;
                case 'e':
                    // Trim end
                    strToTrim.erase(strToTrim.find_last_not_of(trimChar) + 1);
                    break;
                case 'b':
                    // Trim both
                    strToTrim.erase(0, strToTrim.find_first_not_of(trimChar));
                    strToTrim.erase(strToTrim.find_last_not_of(trimChar) + 1);
                    break;
                default:
                    // if raise here, then the source code is wrong
                    throw std::logic_error("Invalid trim type for s_trim operator");
                    break;
            }

            event->setString(strToTrim, targetField);

            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_concat/string1|$ref1/string2|$ref2
base::Expression opBuilderHelperStringConcat(const std::any& definition)
{

    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    if (parameters.size() < 2)
    {
        throw std::runtime_error("Invalid number of parameters for s_concat operator");
    }
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: Parameter must be string or int: ", name)};
    const auto failureTrace2 {
        fmt::format("[{}] -> Failure: not found parameter: ", name)};

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            std::string result {};

            for (auto parameter : parameters)
            {
                if (helper::base::Parameter::Type::REFERENCE == parameter.m_type)
                {
                    // Check path exists
                    if (!event->exists(parameter.m_value))
                    {
                        return base::result::makeFailure(
                            event, failureTrace2 + parameter.m_value);
                    }

                    // Get field value
                    std::string resolvedField;
                    if (event->isDouble(parameter.m_value))
                    {
                        resolvedField =
                            std::to_string(event->getDouble(parameter.m_value).value());
                    }
                    else if (event->isInt(parameter.m_value))
                    {
                        resolvedField =
                            std::to_string(event->getInt(parameter.m_value).value());
                    }
                    else if (event->isString(parameter.m_value))
                    {
                        resolvedField = event->getString(parameter.m_value).value();
                    }
                    else if (event->isObject(parameter.m_value))
                    {
                        resolvedField = event->str(parameter.m_value).value();
                    }
                    else
                    {
                        return base::result::makeFailure(
                            event, failureTrace1 + parameter.m_value);
                    }

                    result.append(resolvedField);
                }
                else
                {
                    result.append(parameter.m_value);
                }
            }

            event->setString(result, targetField);

            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_from_array/$<array_reference1>/<separator>
base::Expression opBuilderHelperStringFromArray(const std::any& definition)
{
    const auto [targetField, name, rawParameters] =
        helper::base::extractDefinition(definition);
    const auto parameters = helper::base::processParameters(rawParameters);
    helper::base::checkParametersSize(parameters, 2);

    // Check Array reference parameter
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);
    const auto arrayName = parameters.at(0);

    // Check separator parameter
    helper::base::checkParameterType(parameters[1], helper::base::Parameter::Type::VALUE);
    const auto separator = parameters.at(1);

    const auto traceName =
        helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", traceName);
    const auto failureTrace1 =
        fmt::format("[{}] -> Failure: Array Member should be a string", traceName);
    const auto failureTrace2 = fmt::format(
        "[{}] -> Failure: parameter is not an array or it doesn't exist", traceName);

    // Return Term
    return base::Term<base::EngineOp>::create(
        traceName,
        [=,
         targetField = std::move(targetField),
         separator = std::move(separator.m_value),
         arrayName = std::move(arrayName.m_value)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // Getting array field, must be a reference
            const auto stringJsonArray = event->getArray(arrayName);
            if (!stringJsonArray.has_value())
            {
                return base::result::makeFailure(event, failureTrace2);
            }

            std::vector<std::string> stringArray;
            for (const auto& s_param : stringJsonArray.value())
            {
                if (s_param.isString())
                {
                    const auto strVal = s_param.getString().value();
                    stringArray.emplace_back(std::move(strVal));
                }
                else
                {
                    return base::result::makeFailure(event, failureTrace1);
                }
            }

            // accumulated concation without trailing indexes
            const std::string composedValueString {
                utils::string::join(stringArray, separator)};

            event->setString(composedValueString, targetField);
            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_from_hexa/$<hex_reference>
base::Expression opBuilderHelperStringFromHexa(const std::any& definition)
{
    const auto [targetField, name, rawParameters] =
        helper::base::extractDefinition(definition);

    const auto parameters = helper::base::processParameters(rawParameters);

    helper::base::checkParametersSize(parameters, 1);

    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);

    const auto sourceField = parameters.at(0);

    const auto traceName =
        helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", traceName);
    const auto failureTrace1 = fmt::format(
        "[{}] -> Failure: parameter is not a string or it doesn't exist", traceName);
    const auto failureTrace2 =
        fmt::format("[{}] -> Failure: parameter is not a reference", traceName);
    const auto failureTrace3 = fmt::format(
        "[{}] -> Failure: hexa string has not a pair number of digits", traceName);

    // Return Term
    return base::Term<base::EngineOp>::create(
        traceName,
        [=, targetField = std::move(targetField), sourceField = std::move(sourceField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            std::string strHex {};

            // Getting string field from a reference
            const auto refStrHEX = event->getString(sourceField.m_value);
            if (!refStrHEX.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            strHex = refStrHEX.value();

            const auto lenHex = strHex.length();

            if (lenHex % 2)
            {
                return base::result::makeFailure(event, failureTrace3);
            }

            std::string strASCII {};
            strASCII.resize((lenHex / 2) + 1);

            for (int iHex = 0, iASCII = 0; iHex < lenHex; iHex += 2, iASCII++)
            {
                char* err = nullptr;

                std::string byte = strHex.substr(iHex, 2);
                char chr = (char)strtol(byte.c_str(), &err, 16); // BASE: 16 (Hexa)

                if (err != nullptr && *err != 0)
                {
                    return base::result::makeFailure(
                        event,
                        fmt::format(
                            "[{}] -> Failure: Character '{}' is not a valid hexa digit",
                            traceName,
                            err));
                }

                strASCII[iASCII] = chr;
            }

            event->setString(strASCII, targetField);

            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_hex_to_num/$ref
base::Expression opBuilderHelperHexToNumber(const std::any& definition)
{
    const auto [targetField, name, rawParameters] =
        helper::base::extractDefinition(definition);
    const auto parameters = helper::base::processParameters(rawParameters);
    helper::base::checkParametersSize(parameters, 1);
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);
    const auto sourceField = parameters.at(0);

    const auto traceName =
        helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", traceName);
    const auto failureTrace1 = fmt::format(
        "[{}] -> Failure: parameter is not a string or it doesn't exist", traceName);
    const auto failureTrace2 =
        fmt::format("[{}] -> Failure: Bad hexadecimal string", traceName);

    // Return Term
    return base::Term<base::EngineOp>::create(
        traceName,
        [=, targetField = std::move(targetField), sourceField = std::move(sourceField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // Getting string field from a reference
            const auto refStrHEX = event->getString(sourceField.m_value);
            if (!refStrHEX.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }
            std::stringstream ss;
            ss << refStrHEX.value();
            int result;
            ss >> std::hex >> result;
            if (ss.fail() || !ss.eof())
            {
                return base::result::makeFailure(event, failureTrace2);
            }

            event->setInt(result, targetField);
            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_replace/old_substring/new_substring
base::Expression opBuilderHelperStringReplace(const std::any& definition)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 2);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    const auto paramOldSubstr = parameters.at(0);
    if (paramOldSubstr.m_value.empty())
    {
        throw std::runtime_error(
            fmt::format("First parameter of '{}' cannot be empty.", name));
    }
    const auto paramNewSubstr = parameters.at(1);

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: [{}] not found", name, targetField)};
    const auto failureTrace2 {
        fmt::format("[{}] -> Failure: [{}] is empty", name, targetField)};

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=,
         targetField = std::move(targetField),
         paramOldSubstr = std::move(paramOldSubstr),
         paramNewSubstr = std::move(paramNewSubstr)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // Get field value
            std::optional<std::string> resolvedField {event->getString(targetField)};

            // Check if field is a string
            if (!resolvedField.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            // Check if field is a string
            if (resolvedField.value().empty())
            {
                return base::result::makeFailure(event, failureTrace2);
            }

            auto newString {resolvedField.value()};

            std::string oldSubstring {paramOldSubstr.m_value};
            if (helper::base::Parameter::Type::REFERENCE == paramOldSubstr.m_type)
            {
                resolvedField = event->getString(paramOldSubstr.m_value);

                // Check if field is a string
                if (!resolvedField.has_value())
                {
                    return base::result::makeFailure(event, failureTrace1);
                }

                // Check if field is a string
                if (resolvedField.value().empty())
                {
                    return base::result::makeFailure(event, failureTrace2);
                }

                oldSubstring = resolvedField.value();
            }

            std::string newSubstring {paramNewSubstr.m_value};
            if (helper::base::Parameter::Type::REFERENCE == paramNewSubstr.m_type)
            {
                resolvedField = event->getString(paramNewSubstr.m_value);

                // Check if field is a string
                if (!resolvedField.has_value())
                {
                    return base::result::makeFailure(event, failureTrace1);
                }

                // Check if field is a string
                if (resolvedField.value().empty())
                {
                    return base::result::makeFailure(event, failureTrace2);
                }

                newSubstring = resolvedField.value();
            }

            size_t start_pos = 0;
            while ((start_pos = newString.find(oldSubstring, start_pos))
                   != std::string::npos)
            {
                newString.replace(start_pos, oldSubstring.length(), newSubstring);
                start_pos += newSubstring.length();
            }

            event->setString(newString, targetField);

            return base::result::makeSuccess(event, successTrace);
        });
}

//*************************************************
//*           Int tranform                        *
//*************************************************

// field: +i_calc/[+|-|*|/]/val|$ref/
base::Expression opBuilderHelperIntCalc(const std::any& definition)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 2);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);
    const auto op {strToOp(parameters[0])};

    auto expression {
        opBuilderHelperIntTransformation(targetField, op, parameters[1], name)};
    return expression;
}

//*************************************************
//*           Regex tranform                      *
//*************************************************

// field: +r_ext/_field/regexp/
base::Expression opBuilderHelperRegexExtract(const std::any& definition)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 2);
    // Parameter type check
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);
    helper::base::checkParameterType(parameters[1], helper::base::Parameter::Type::VALUE);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    std::string map_field {parameters[0].m_value};

    auto regex_ptr {std::make_shared<RE2>(parameters[1].m_value)};
    if (!regex_ptr->ok())
    {
        const std::string err = "Error compiling regex '" + parameters[1].m_value + "'. "
                                + regex_ptr->error();
        throw std::runtime_error(err);
    }

    // Tracing
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {
        fmt::format("[{}] -> Failure: [{}] not found", name, targetField)};
    const auto failureTrace2 {fmt::format("[{}] -> Failure", name)};
    const auto failureTrace3 {
        fmt::format("[{}] -> Failure: [{}] not found", name, map_field)};

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // TODO Remove try catch
            auto resolvedField {event->getString(map_field)};

            if (!resolvedField.has_value())
            {
                return base::result::makeFailure(event, failureTrace3);
            }

            std::string match;
            if (RE2::PartialMatch(resolvedField.value(), *regex_ptr, &match))
            {
                event->setString(match, targetField);

                return base::result::makeSuccess(event, successTrace);
            }
            return base::result::makeFailure(event, failureTrace2);
        });
}

//*************************************************
//*           Array tranform                      *
//*************************************************

// field: +a_append/$field|literal...
base::Expression opBuilderHelperAppend(const std::any& definition)
{
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    auto parameters = helper::base::processParameters(rawParameters);
    if (parameters.empty())
    {
        throw std::runtime_error(
            fmt::format("[opBuilderHelperAppend] parameters can not be empty"));
    }
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", name);

    const auto failureTrace1 =
        fmt::format("[{}] -> Failure: parameter reference not found", name);

    // Return result
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            for (const auto& parameter : parameters)
            {
                switch (parameter.m_type)
                {
                    case helper::base::Parameter::Type::REFERENCE:
                    {
                        auto value = event->getJson(parameter.m_value);
                        if (!value)
                        {
                            return base::result::makeFailure(event, failureTrace1);
                        }

                        event->appendJson(value.value(), targetField);
                    }
                    break;
                    case helper::base::Parameter::Type::VALUE:
                    {
                        event->appendString(parameter.m_value, targetField);
                    }
                    break;
                    default:
                        throw std::runtime_error(
                            fmt::format("{}: unexpected parameter type [{}]",
                                        name,
                                        parameter.m_value));
                }
            }
            return base::result::makeSuccess(event, successTrace);
        });
}

// field: +s_to_array/$field/[,| | ...]
base::Expression opBuilderHelperAppendSplitString(const std::any& definition)
{
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    auto parameters = helper::base::processParameters(rawParameters);
    helper::base::checkParametersSize(parameters, 2);
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);
    helper::base::checkParameterType(parameters[1], helper::base::Parameter::Type::VALUE);
    if (parameters[1].m_value.size() != 1)
    {
        throw std::runtime_error(fmt::format(
            "[opBuilderHelperAppendSplit] separator can only be one character"));
    }

    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", name);

    const auto failureTrace1 =
        fmt::format("[{}] -> Failure: parameter reference [{}] not found or not string",
                    name,
                    parameters[0].m_value);

    // Return result
    return base::Term<base::EngineOp>::create(
        name,
        [=,
         targetField = std::move(targetField),
         fieldReference = std::move(parameters[0].m_value),
         separator = std::move(parameters[1].m_value[0])](
            base::Event event) -> base::result::Result<base::Event>
        {
            auto resolvedReference = event->getString(fieldReference);
            if (!resolvedReference.has_value())
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            auto splitted = utils::string::split(resolvedReference.value(), separator);

            for (const auto& value : splitted)
            {
                event->appendString(value, targetField);
            }

            return base::result::makeSuccess(event, successTrace);
        });
}

base::Expression opBuilderHelperMerge(const std::any& definition)
{
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    auto parameters = helper::base::processParameters(rawParameters);
    helper::base::checkParametersSize(parameters, 1);
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);

    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", name);

    const auto failureTrace1 =
        fmt::format("[{}] -> Failure: parameter reference [{}] not found",
                    name,
                    parameters[0].m_value);
    const auto failereTrace2 =
        fmt::format("[{}] -> Failure: target field [{}] not found", name, targetField);
    const auto failureTrace3 = fmt::format("[{}] -> Failure: fields type error", name);

    // Return result
    return base::Term<base::EngineOp>::create(
        name,
        [=,
         targetField = std::move(targetField),
         fieldReference = std::move(parameters[0].m_value)](
            base::Event event) -> base::result::Result<base::Event>
        {
            // Check target and reference field exists
            if (!event->exists(fieldReference))
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            if (!event->exists(targetField))
            {
                return base::result::makeFailure(event, failereTrace2);
            }

            // Check fields types
            auto tt = event->type(targetField);
            auto st = event->type(fieldReference);
            if (event->type(targetField) != event->type(fieldReference)
                || (event->type(targetField) != json::Json::Type::Array
                    && event->type(targetField) != json::Json::Type::Object))
            {
                return base::result::makeFailure(event, failureTrace3);
            }

            // Merge
            event->merge(fieldReference, targetField);

            return base::result::makeSuccess(event, successTrace);
        });
}

//*************************************************
//*             JSON tranform                     *
//*************************************************

// field: +ef_delete
base::Expression opBuilderHelperDeleteField(const std::any& definition)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number of parameters
    helper::base::checkParametersSize(parameters, 0);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace {fmt::format("[{}] -> Failure", name)};

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            if (event->erase(targetField))
            {
                return base::result::makeSuccess(event, successTrace);
            }
            else
            {
                return base::result::makeFailure(event, failureTrace);
            }
        });
}

// field: +ef_rename/$sourceField
base::Expression opBuilderHelperRenameField(const std::any& definition)
{
    // Extract parameters from any
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    // Identify references and build JSON pointer paths
    auto parameters {helper::base::processParameters(rawParameters)};
    // Assert expected number and type of parameters
    helper::base::checkParametersSize(parameters, 1);
    auto sourceField = parameters[0];
    helper::base::checkParameterType(sourceField,
                                     helper::base::Parameter::Type::REFERENCE);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing messages
    const auto successTrace {fmt::format("[{}] -> Success", name)};
    const auto failureTrace1 {fmt::format(
        "[{}] -> Failure: field '{}' does not exist.", name, sourceField.m_value)};
    const auto failureTrace2 {fmt::format(
        "[{}] -> Failure: field '{}' could not be removed.", name, sourceField.m_value)};

    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField), sourceField = std::move(sourceField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            if (event->exists(sourceField.m_value))
            {
                event->set(targetField, sourceField.m_value);
            }
            else
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            if (event->erase(sourceField.m_value))
            {
                return base::result::makeSuccess(event, successTrace);
            }
            else
            {
                return base::result::makeFailure(event, failureTrace2);
            }
        });
}

//*************************************************
//*              IP tranform                      *
//*************************************************
// field: +s_IPVersion/$ip_field
base::Expression opBuilderHelperIPVersionFromIPStr(const std::any& definition)
{
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    auto parameters {helper::base::processParameters(rawParameters)};

    // Check parameters
    helper::base::checkParametersSize(parameters, 1);
    helper::base::checkParameterType(parameters[0],
                                     helper::base::Parameter::Type::REFERENCE);

    // Tracing
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    const auto successTrace {fmt::format("[{}] -> Success", name)};

    const auto failureTrace1 {fmt::format(
        "[{}] -> Failure: parameter reference not found or not a string", name)};
    const auto failureTrace2 {
        fmt::format("[{}] -> Failure: The string not a valid IP address", name)};

    // Return result
    return base::Term<base::EngineOp>::create(
        name,
        [=,
         targetField = std::move(targetField),
         name = std::move(name),
         ipStrPath = std::move(parameters[0].m_value)](
            base::Event event) -> base::result::Result<base::Event>
        {
            const auto strIP = event->getString(ipStrPath);

            if (!strIP)
            {
                return base::result::makeFailure(event, failureTrace1);
            }

            if (utils::ip::checkStrIsIPv4(strIP.value()))
            {
                event->setString("IPv4", targetField);
            }
            else if (utils::ip::checkStrIsIPv6(strIP.value()))
            {
                event->setString("IPv6", targetField);
            }
            else
            {
                return base::result::makeFailure(event, failureTrace2);
            }
            return base::result::makeSuccess(event, successTrace);
        });
}

//*************************************************
//*              Time tranform                    *
//*************************************************

// field: + sys_epoch
base::Expression opBuilderHelperEpochTimeFromSystem(const std::any& definition) {
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    auto parameters {helper::base::processParameters(rawParameters)};

    // Check parameters
    helper::base::checkParametersSize(parameters, 0);

    // Tracing
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    const auto successTrace {fmt::format("[{}] -> Success", name)};
    const auto failureTrace {fmt::format("[{}] -> Failure (overflow)", name)};

    // Return result
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField)](
            base::Event event) -> base::result::Result<base::Event>
        {
            auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
            // TODO: Delete this and dd SetInt64 or SetIntAny to JSON class, get
            // Number of any type (fix concat helper)
            if (sec > std::numeric_limits<int>::max())
            {
                return base::result::makeFailure(event, failureTrace);
            }
            event->setInt(sec, targetField);
            return base::result::makeSuccess(event, successTrace);
        });
}

//*************************************************
//*              Checksum and hash                *
//*************************************************

// field: +h_sha1/<string1>|<string_reference1>
base::Expression opBuilderHelperHashSHA1(const std::any& definition)
{
    auto [targetField, name, rawParameters] = helper::base::extractDefinition(definition);
    const auto parameters = helper::base::processParameters(rawParameters);

    // Assert expected minimun number of parameters
    helper::base::checkParametersSize(parameters, 1);
    // Format name for the tracer
    name = helper::base::formatHelperFilterName(name, targetField, parameters);

    // Tracing
    const auto successTrace = fmt::format("[{}] -> Success", name);
    const auto failureTrace1 =
        fmt::format("[{}] -> Failure: Argument shouldn't be empty", name);
    const auto failureTrace2 = fmt::format(
        "[{}] -> Failure: Couldn't create HASH and write it in the JSON", name);

    // Return Term
    return base::Term<base::EngineOp>::create(
        name,
        [=, targetField = std::move(targetField), parameter = std::move(parameters.at(0))](
            base::Event event) -> base::result::Result<base::Event>
        {
            std::string resolvedParameter;
            // Check parameter
            if (helper::base::Parameter::Type::REFERENCE == parameter.m_type)
            {
                const auto paramValue {event->getString(parameter.m_value)};
                if (!paramValue.has_value())
                {
                    return base::result::makeFailure(event, failureTrace1);
                }
                resolvedParameter = paramValue.value();
            }
            else
            {
                resolvedParameter = parameter.m_value;
            }

            const auto resultHash = hashStringSHA1(resolvedParameter);
            if (!resultHash.has_value())
            {
                return base::result::makeFailure(event, failureTrace2);
            }
            event->setString(resultHash.value(), targetField);
            return base::result::makeSuccess(event, successTrace);
        });
}

} // namespace builder::internals::builders
/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <string>

#include "mongo/db/commands.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"

namespace mongo {
namespace {

class CmdAnalyze final : public TypedCommand<CmdAnalyze> {
public:
    using Request = AnalyzeCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to generate statistics for a collection for use in the optimizer.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void typedRun(OperationContext* opCtx) {
            uassert(6660400,
                    "Analyze command requires common query framework feature flag to be enabled",
                    serverGlobalParams.featureCompatibility.isVersionInitialized() &&
                        feature_flags::gFeatureFlagCommonQueryFramework.isEnabled(
                            serverGlobalParams.featureCompatibility));

            uasserted(ErrorCodes::NotImplemented, "Analyze command not yet implemented");
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            // TODO SERVER-67656
        }
    };

} cmdAnalyze;

}  // namespace
}  // namespace mongo

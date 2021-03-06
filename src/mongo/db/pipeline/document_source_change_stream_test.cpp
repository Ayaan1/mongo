/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/intrusive_ptr.hpp>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using boost::intrusive_ptr;
using repl::OpTypeEnum;
using repl::OplogEntry;
using std::list;
using std::string;
using std::vector;

using D = Document;
using V = Value;

using DSChangeStream = DocumentSourceChangeStream;

static const Timestamp ts(100, 1);
static const repl::OpTime optime(ts, 1);
static const NamespaceString nss("unittests.change_stream");

using ChangeStreamStageTestNoSetup = AggregationContextFixture;

class ChangeStreamStageTest : public AggregationContextFixture {
public:
    ChangeStreamStageTest() : AggregationContextFixture(nss) {
        repl::ReplicationCoordinator::set(getExpCtx()->opCtx->getServiceContext(),
                                          stdx::make_unique<repl::ReplicationCoordinatorMock>(
                                              getExpCtx()->opCtx->getServiceContext()));
    }

    void checkTransformation(const OplogEntry& entry, const boost::optional<Document> expectedDoc) {
        const auto spec = fromjson("{$changeNotification: {}}");
        list<intrusive_ptr<DocumentSource>> result =
            DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());

        auto match = dynamic_cast<DocumentSourceMatch*>(result.front().get());
        ASSERT(match);
        auto mock = DocumentSourceMock::create(D(entry.toBSON()));
        match->setSource(mock.get());

        // Check the oplog entry is transformed correctly.
        auto transform = result.back().get();
        ASSERT(transform);
        ASSERT_EQ(string(transform->getSourceName()), DSChangeStream::kStageName);
        transform->setSource(match);

        auto next = transform->getNext();
        // Match stage should pass the doc down if expectedDoc is given.
        ASSERT_EQ(next.isAdvanced(), static_cast<bool>(expectedDoc));
        if (expectedDoc) {
            ASSERT_DOCUMENT_EQ(next.releaseDocument(), *expectedDoc);
        }
    }

    OplogEntry createCommand(const BSONObj& oField) {
        return OplogEntry(optime, 1, OpTypeEnum::kCommand, nss.getCommandNS(), oField);
    }
};

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("unexpected" << 4)).firstElement(), expCtx),
        UserException,
        40415);
}

TEST_F(ChangeStreamStageTest, ShouldRejectNonStringFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(
            BSON(DSChangeStream::kStageName << BSON("fullDocument" << true)).firstElement(),
            expCtx),
        UserException,
        ErrorCodes::TypeMismatch);
}

TEST_F(ChangeStreamStageTest, ShouldRejectUnrecognizedFullDocumentOption) {
    auto expCtx = getExpCtx();

    ASSERT_THROWS_CODE(
        DSChangeStream::createFromBson(BSON(DSChangeStream::kStageName << BSON("fullDocument"
                                                                               << "unrecognized"))
                                           .firstElement(),
                                       expCtx),
        UserException,
        40575);
}

TEST_F(ChangeStreamStageTestNoSetup, FailsWithNoReplicationCoordinator) {
    const auto spec = fromjson("{$changeStream: {}}");

    ASSERT_THROWS_CODE(DocumentSourceChangeStream::createFromBson(spec.firstElement(), getExpCtx()),
                       UserException,
                       40573);
}

TEST_F(ChangeStreamStageTest, StagesGeneratedCorrectly) {
    const auto spec = fromjson("{$changeStream: {}}");

    list<intrusive_ptr<DocumentSource>> result =
        DSChangeStream::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_EQUALS(result.size(), 2UL);
    ASSERT_TRUE(dynamic_cast<DocumentSourceMatch*>(result.front().get()));
    ASSERT_EQUALS(string(result.front()->getSourceName()), DSChangeStream::kStageName);
    ASSERT_EQUALS(string(result.back()->getSourceName()), DSChangeStream::kStageName);

    // TODO: Check explain result.
}

TEST_F(ChangeStreamStageTest, TransformInsert) {
    OplogEntry insert(optime, 1, OpTypeEnum::kInsert, nss, BSON("_id" << 1 << "x" << 1));
    // Insert
    Document expectedInsert{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInsertOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"x", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(insert, expectedInsert);
}

TEST_F(ChangeStreamStageTest, TransformUpdateFields) {
    OplogEntry updateField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$set" << BSON("y" << 1)), BSON("_id" << 1));
    // Update fields
    Document expectedUpdateField{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kFullDocumentField, BSONNULL},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{{"y", 1}}}, {"removedFields", vector<V>()}},
        },
    };
    checkTransformation(updateField, expectedUpdateField);
}

TEST_F(ChangeStreamStageTest, TransformRemoveFields) {
    OplogEntry removeField(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("$unset" << BSON("y" << 1)), BSON("_id" << 1));
    // Remove fields
    Document expectedRemoveField{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kUpdateOpType},
        {DSChangeStream::kFullDocumentField, BSONNULL},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
        {
            "updateDescription", D{{"updatedFields", D{}}, {"removedFields", vector<V>{V("y"_sd)}}},
        }};
    checkTransformation(removeField, expectedRemoveField);
}

TEST_F(ChangeStreamStageTest, TransformReplace) {
    OplogEntry replace(
        optime, 1, OpTypeEnum::kUpdate, nss, BSON("_id" << 1 << "y" << 1), BSON("_id" << 1));
    // Replace
    Document expectedReplace{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kReplaceOpType},
        {DSChangeStream::kFullDocumentField, D{{"_id", 1}, {"y", 1}}},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(replace, expectedReplace);
}

TEST_F(ChangeStreamStageTest, TransformDelete) {
    OplogEntry deleteEntry(optime, 1, OpTypeEnum::kDelete, nss, BSON("_id" << 1));
    // Delete
    Document expectedDelete{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.ns()}, {"_id", 1}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kDeleteOpType},
        {DSChangeStream::kFullDocumentField, BSONNULL},
        {DSChangeStream::kNamespaceField, D{{"db", nss.db()}, {"coll", nss.coll()}}},
        {DSChangeStream::kDocumentKeyField, D{{"_id", 1}}},
    };
    checkTransformation(deleteEntry, expectedDelete);
}

TEST_F(ChangeStreamStageTest, TransformInvalidate) {
    NamespaceString otherColl("test.bar");

    OplogEntry dropColl = createCommand(BSON("drop" << nss.coll()));
    OplogEntry dropDB = createCommand(BSON("dropDatabase" << 1));
    OplogEntry rename =
        createCommand(BSON("renameCollection" << nss.ns() << "to" << otherColl.ns()));

    // Invalidate entry includes $cmd namespace in _id and doesn't have a document id.
    Document expectedInvalidate{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", nss.getCommandNS().ns()}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kFullDocumentField, BSONNULL},
    };
    for (auto& entry : {dropColl, dropDB, rename}) {
        checkTransformation(entry, expectedInvalidate);
    }
}

TEST_F(ChangeStreamStageTest, TransformInvalidateRenameDropTarget) {
    // renameCollection command with dropTarget: true has the namespace of the "from" database.
    NamespaceString otherColl("test.bar");
    OplogEntry rename(optime,
                      1,
                      OpTypeEnum::kCommand,
                      otherColl.getCommandNS(),
                      BSON("renameCollection" << otherColl.ns() << "to" << nss.ns()));
    Document expectedInvalidate{
        {DSChangeStream::kIdField, D{{"ts", ts}, {"ns", otherColl.getCommandNS().ns()}}},
        {DSChangeStream::kOperationTypeField, DSChangeStream::kInvalidateOpType},
        {DSChangeStream::kFullDocumentField, BSONNULL},
    };
    checkTransformation(rename, expectedInvalidate);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateCollection) {
    auto collSpec =
        D{{"create", "foo"_sd},
          {"idIndex", D{{"v", 2}, {"key", D{{"_id", 1}}}, {"name", "_id_"_sd}, {"ns", nss.ns()}}}};
    OplogEntry createColl = createCommand(collSpec.toBson());
    checkTransformation(createColl, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersNoOp) {
    OplogEntry noOp(
        optime, 1, OpTypeEnum::kNoop, NamespaceString(), fromjson("{'msg':'new primary'}"));
    checkTransformation(noOp, boost::none);
}

TEST_F(ChangeStreamStageTest, MatchFiltersCreateIndex) {
    auto indexSpec = D{{"v", 2}, {"key", D{{"a", 1}}}, {"name", "a_1"_sd}, {"ns", nss.ns()}};
    NamespaceString indexNs(nss.getSystemIndexesCollection());
    OplogEntry createIndex(optime, 1, OpTypeEnum::kInsert, indexNs, indexSpec.toBson());
    checkTransformation(createIndex, boost::none);
}

TEST_F(ChangeStreamStageTest, TransformationShouldBeAbleToReParseSerializedStage) {
    auto expCtx = getExpCtx();

    auto originalSpec = BSON(DSChangeStream::kStageName << BSONObj());
    auto allStages = DSChangeStream::createFromBson(originalSpec.firstElement(), expCtx);
    ASSERT_EQ(allStages.size(), 2UL);
    auto stage = allStages.back();
    ASSERT(dynamic_cast<DocumentSourceSingleDocumentTransformation*>(stage.get()));

    //
    // Serialize the stage and confirm contents.
    //
    vector<Value> serialization;
    stage->serializeToArray(serialization);
    ASSERT_EQ(serialization.size(), 1UL);
    ASSERT_EQ(serialization[0].getType(), BSONType::Object);
    auto serializedDoc = serialization[0].getDocument();
    ASSERT_BSONOBJ_EQ(serializedDoc.toBson(), originalSpec);

    //
    // Create a new stage from the serialization. Serialize the new stage and confirm that it is
    // equivalent to the original serialization.
    //
    auto serializedBson = serializedDoc.toBson();
    auto roundTripped = uassertStatusOK(Pipeline::create(
        DSChangeStream::createFromBson(serializedBson.firstElement(), expCtx), expCtx));

    auto newSerialization = roundTripped->serialize();

    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

}  // namespace
}  // namespace mongo

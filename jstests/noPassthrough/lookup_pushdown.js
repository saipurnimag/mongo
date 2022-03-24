/**
 * Tests basic functionality of pushing $lookup into the find layer.
 *
 * @tags: [requires_sharding]
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");      // For 'checkSBEEnabled()'.
load("jstests/libs/analyze_plan.js");  // For 'getAggPlanStages()' and 'hasRejectedPlans()'

const JoinAlgorithm = {
    Classic: 0,
    NLJ: 1,
    INLJ: 2,
    HJ: 3,
    // These joins aren't implemented yet and will throw errors with the corresponding codes.
    INLJHashedIndex: 6357203,
};

// Standalone cases.
const conn = MongoRunner.runMongod({
    setParameter: {featureFlagSBELookupPushdown: true, featureFlagSBELookupPushdownIndexJoin: true}
});
assert.neq(null, conn, "mongod was unable to start up");
const name = "lookup_pushdown";
const foreignCollName = "foreign_lookup_pushdown";
const viewName = "view_lookup_pushdown";

/**
 * Helper function which verifies that at least one $lookup was lowered into SBE within
 * 'explain', and that the EqLookupNode at 'eqLookupNodeIndex' chose the appropriate strategy.
 * In particular, if 'IndexedLoopJoin' was chosen, we verify that the index described by
 * 'indexKeyPattern' was chosen. Otherwise, we verify that 'NestedLoopJoin' was chosen.
 */
function verifyEqLookupNodeStrategy(
    explain, eqLookupNodeIndex, expectedStrategy, indexKeyPattern = {}) {
    const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");
    assert.gt(
        eqLookupNodes.length, 0, "expected at least one EQ_LOOKUP node; got " + tojson(explain));

    // Verify that we're selecting an EQ_LOOKUP node within range.
    assert(eqLookupNodeIndex >= 0 && eqLookupNodeIndex < eqLookupNodes.length,
           "expected eqLookupNodeIndex of '" + eqLookupNodeIndex +
               "' to be within range of available EQ_LOOKUP nodes; got " + tojson(explain));

    // Fetch the requested EQ_LOOKUP node.
    const eqLookupNode = eqLookupNodes[eqLookupNodes.length - 1 - eqLookupNodeIndex];
    assert(eqLookupNode, "expected EQ_LOOKUP node; explain: " + tojson(explain));
    const strategy = eqLookupNode.strategy;
    assert(strategy, "expected EQ_LOOKUP node to have a strategy " + tojson(eqLookupNode));
    assert.eq(
        expectedStrategy,
        strategy,
        "Incorrect strategy; expected " + tojson(expectedStrategy) + ", got " + tojson(strategy));

    if (strategy === "IndexedLoopJoin") {
        assert(indexKeyPattern,
               "expected indexKeyPattern should be set for IndexedLoopJoin algorithm");
        assert.docEq(eqLookupNode.indexKeyPattern,
                     indexKeyPattern,
                     "expected IndexedLoopJoin node to have index " + tojson(indexKeyPattern) +
                         ", got plan " + tojson(eqLookupNode));
    }
}

function getJoinAlgorithmStrategyName(joinAlgorithm) {
    switch (joinAlgorithm) {
        case JoinAlgorithm.NLJ:
            return "NestedLoopJoin";
        case JoinAlgorithm.INLJ:
        case JoinAlgorithm.INLJHashedIndex:
            return "IndexedLoopJoin";
        case JoinAlgorithm.HJ:
            return "HashJoin";
        case JoinAlgorithm.Classic:
        default:
            assert(false, "No strategy for JoinAlgorithm: " + joinAlgorithm);
    }
}

function runTest(coll,
                 pipeline,
                 expectedJoinAlgorithm,
                 indexKeyPattern = null,
                 aggOptions = {},
                 errMsgRegex = null,
                 checkMultiPlanning = false,
                 eqLookupNodeIndex = 0) {
    const options = Object.assign({pipeline, cursor: {}}, aggOptions);
    const response = coll.runCommand("aggregate", options);

    if (expectedJoinAlgorithm === JoinAlgorithm.Classic) {
        assert.commandWorked(response);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const eqLookupNodes = getAggPlanStages(explain, "EQ_LOOKUP");

        // In the classic case, verify that $lookup was not lowered into SBE. Note that we don't
        // check for the presence of $lookup agg stages because in the sharded case, $lookup will
        // not execute on each shard and will not show up in the output of 'getAggPlanStages'.
        assert.eq(eqLookupNodes.length,
                  0,
                  "there should be no lowered EQ_LOOKUP stages; got " + tojson(explain));
    } else if (expectedJoinAlgorithm === JoinAlgorithm.INLJHashedIndex) {
        const result = assert.commandFailedWithCode(response, expectedJoinAlgorithm);
        if (errMsgRegex) {
            const errorMessage = result.errmsg;
            assert(errMsgRegex.test(errorMessage),
                   "Error message '" + errorMessage + "' did not match the RegEx '" + errMsgRegex +
                       "'");
        }
    } else {
        assert.commandWorked(response);
        const explain = coll.explain().aggregate(pipeline, aggOptions);
        const expectedStrategy = getJoinAlgorithmStrategyName(expectedJoinAlgorithm);
        verifyEqLookupNodeStrategy(explain, eqLookupNodeIndex, expectedStrategy, indexKeyPattern);

        // Verify that multiplanning took place by verifying that there was at least one
        // rejected plan.
        if (checkMultiPlanning) {
            assert(hasRejectedPlans(explain), explain);
        }
    }
}

let db = conn.getDB(name);
if (!checkSBEEnabled(db, ["featureFlagSBELookupPushdown"])) {
    jsTestLog("Skipping test because either the sbe lookup pushdown feature flag is disabled or" +
              " sbe itself is disabled");
    MongoRunner.stopMongod(conn);
    return;
}

let coll = db[name];
const localDocs = [{_id: 1, a: 2}];
assert.commandWorked(coll.insert(localDocs));
let foreignColl = db[foreignCollName];
const foreignDocs = [{_id: 0, b: 2, c: 2}];
assert.commandWorked(foreignColl.insert(foreignDocs));
assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
let view = db[viewName];

(function testLookupPushdownBasicCases() {
    // Basic $lookup.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // $lookup against a non-existent foreign collection should always pick NLJ.
    runTest(coll,
            [{$lookup: {from: "nonexistent", localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // Self join $lookup, no views.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // Self join $lookup; left hand is a view. This is expected to be pushed down because the view
    // pipeline itself is a $match, which is eligible for pushdown.
    runTest(view,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // Self join $lookup; right hand is a view.
    runTest(coll,
            [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Self join $lookup; both namespaces are views.
    runTest(view,
            [{$lookup: {from: viewName, localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup preceded by $match.
    runTest(coll,
            [
                {$match: {a: {$gte: 0}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // $lookup preceded by $project.
    runTest(coll,
            [
                {$project: {a: 1}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // $lookup preceded by $project which features an SBE-incompatible expression.
    // TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
    runTest(coll,
            [
                {$project: {exp: {$pow: ["$a", 3]}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup preceded by $group.
    runTest(coll,
            [
                {$group: {_id: "$a", sum: {$sum: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // $lookup preceded by $group that is not eligible for pushdown.
    // TODO SERVER-51542: Update or remove this test case once $pow is implemented in SBE.
    runTest(coll,
            [
                {$group: {_id: {$pow: ["$a", 3]}, sum: {$sum: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Consecutive $lookups, where the first $lookup is against a view.
    runTest(coll,
            [
                {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Consecutive $lookups, where the first $lookup is against a regular collection. Here, neither
    // $lookup is eligible for pushdown because currently, we can only know whether any secondary
    // collection is a view or a sharded collection.
    runTest(coll,
            [
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
                {$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup with pipeline.
    runTest(coll,
            [{
                $lookup: {
                    from: foreignCollName, let: {foo: "$b"}, pipeline: [{
                        $match: {
                            $expr: {
                                $eq: ["$$foo",
                                    2]
                            }
                        }
                    }], as: "out"
                }
            }], JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup that absorbs $unwind.
    runTest(coll,
            [
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
                {$unwind: "$out"}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup that absorbs $match.
    runTest(coll,
            [
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
                {$unwind: "$out"},
                {$match: {out: {$gte: 0}}}
            ],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // $lookup that does not absorb $match.
    runTest(coll,
            [
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}},
                {$match: {out: {$gte: 0}}}
            ],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // Run a $lookup with 'allowDiskUse' enabled. Because the foreign collection is very small, we
    // should select hash join.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.HJ /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {allowDiskUse: true});
}());

// Build an index on the foreign collection that matches the foreignField. This should cause us
// to choose an indexed nested loop join.
(function testIndexNestedLoopJoinRegularIndex() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a hashed index on the foreign collection that matches the foreignField. Indexed nested loop
// join strategy should be used.
(function testIndexNestedLoopJoinHashedIndex() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJHashedIndex /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {} /* aggOptions */,
            /b_hashed//* errMsgRegex */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a wildcard index on the foreign collection that matches the foreignField. Nested loop join
// strategy should be used.
(function testWildcardIndexInhibitsIndexNestedLoopJoin() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({'$**': 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build a compound index that is prefixed with the foreignField. We should use an indexed
// nested loop join.
(function testCompoundIndexWithForeignFieldPrefix() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, c: 1, a: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
})();

// Build multiple compound indexes prefixed with the foreignField. We should utilize the index with
// the least amount of components.
(function testIndexWithFewestComponentsIsUsed() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, a: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

(function testBTreeIndexChosenOverHashedIndex() {
    // In the presence of hashed and BTree indexes with the same number of components, we should
    // select BTree one.
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// While selecting a BTree index is more preferable, we should favor hashed index if it has
// smaller number of components.
(function testFewerComponentsFavoredOverIndexType() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1, d: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 'hashed'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJHashedIndex /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {} /* aggOptions */,
            /b_hashed//* errMsgRegex */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// If we have two indexes of the same type with the same number of components, index keypattern
// should be used as a tie breaker.
(function testIndexKeyPatternUsedAsTieBreaker() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1, c: 1}));
    assert.commandWorked(foreignColl.createIndex({b: 1, a: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1, a: 1});
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Build a 2d index on the foreign collection that matches the foreignField. In this case, we should
// use regular nested loop join.
(function testNonBTreeOrHashedIndexesNotUsedForPushdown() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: '2d'}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Build a compound index containing the foreignField, but not as the first field. In this case,
// we should use regular nested loop join.
(function testForeignFieldNotPrefixInhibitsIndexNestedLoopJoin() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({a: 1, b: 1, c: 1}));
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
    assert.commandWorked(foreignColl.dropIndexes());
}());

// Multiple $lookup stages in a pipeline that should pick different physical joins.
(function testMultipleLookupStagesPickDifferentPhysicalJoins() {
    assert.commandWorked(foreignColl.dropIndexes());
    assert.commandWorked(foreignColl.createIndex({b: 1}));

    let pipeline = [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
    ];
    runTest(coll, pipeline, JoinAlgorithm.INLJ /* expectedJoinAlgorithm */, {
        b: 1
    } /* indexKeyPattern */);
    runTest(coll,
            pipeline,
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */,
            null /* indexKeyPattern */,
            {} /* aggOptions */,
            null /* errMsgRegex */,
            false /* checkMultiPlanning */,
            1 /* eqLookupNodeIndex */);

    pipeline = [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
    ];
    runTest(coll, pipeline, JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
    runTest(coll,
            pipeline,
            JoinAlgorithm.INLJ /* expectedJoinAlgorithm */,
            {b: 1} /* indexKeyPattern */,
            {} /* aggOptions */,
            null /* errMsgRegex */,
            false /* checkMultiPlanning */,
            1 /* eqLookupNodeIndex */);

    assert.commandWorked(foreignColl.dropIndexes());
})();

(function testNumericComponentsBehaviorForPushdown() {
    // "localField" contains a numeric component (unsupported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a.0", foreignField: "a", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // "foreignField" contains a numeric component (unsupported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a.0", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // "as" field contains a numeric component (numbers in this field are treated as literal field
    // names so this is supported by SBE).
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out.0"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
}());

// Until SERVER-63690 is implemented, lowering of $lookup with paths into SBE is disabled.
(function testLocalOrForeignFieldsWithPaths() {
    // "localField" is a path.
    runTest(coll,
            [{$lookup: {from: name, localField: "a.b", foreignField: "a", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // "foreignField" is a path.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a.b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // SERVER-63690 doesn't apply to the "as" field.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "a", as: "out.b"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);
}());

// Verify that $lookup pushdown works correctly in the presence of multi-planning.
(
    function testLookupPushdownWorksWithMultiplanning() {
        assert.commandWorked(coll.dropIndexes());
        assert.commandWorked(coll.createIndexes([{a: 1, b: 1}, {a: 1, c: 1}]));

        // Verify that $lookup still gets pushed down when the pipeline prefix is pushed down and
        // undergoes multi-planning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}}
            ],
            JoinAlgorithm.NLJ, /* expectedJoinAlgorithm */
            null,              /* indexKeyPattern */
            {},                /* aggOptions */
            null,              /* errMsgRegex */
            true /* checkMultiplanning */);

        // Verify that multiple $lookups will still get pushed down when the pipeline prefix is
        // pushed down and undergoes multi-planning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "b_out"}}
            ],
            JoinAlgorithm.NLJ, /* expectedJoinAlgorithm */
            null,              /* indexKeyPattern */
            {},                /* aggOptions */
            null,              /* errMsgRegex */
            true /* checkMultiplanning */);

        // Verify that $lookup and $group both get pushed down in the presence of multiplanning.
        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$group: {_id: "$a", groupOut: {$sum: 1}}},
                {
                    $lookup: {
                        from: foreignCollName,
                        localField: "groupOut",
                        foreignField: "c",
                        as: "c_out"
                    }
                }
            ],
            JoinAlgorithm.NLJ, /* expectedJoinAlgorithm */
            null, /* indexKeyPattern */
            {},                /* aggOptions */
            null,              /* errMsgRegex */
            true /* checkMultiplanning */);

        runTest(
            coll,
            [
                {$match: {a: {$gt: 1}}},
                {$lookup: {from: foreignCollName, localField: "a", foreignField: "c", as: "c_out"}},
                {$group: {_id: "$c_out", groupOut: {$sum: 1}}},
            ],
            JoinAlgorithm.NLJ, /* expectedJoinAlgorithm */
            null,              /* indexKeyPattern */
            {},                /* aggOptions */
            null,              /* errMsgRegex */
            true /* checkMultiplanning */);
        assert.commandWorked(coll.dropIndexes());
    })();

// Verify that $lookup is correctly pushed down when it is nested inside of a $unionWith.
(
    function
        verifyLookupNestedInUnionWithGetsPushedDown() {
            const unionCollName = "unionColl";
            const unionColl = db[unionCollName];
            assert.commandWorked(unionColl.insert({}));
            const explain = coll.explain().aggregate([{$unionWith: {coll: unionCollName, pipeline: [{$lookup: {from:
                foreignCollName, localField: "a", foreignField: "b", as: "results"}}]}}]);
            const unionWithStage = getAggPlanStage(explain, "$unionWith");
            const unionWithSpec = unionWithStage["$unionWith"];
            assert(unionWithSpec.hasOwnProperty("pipeline"), unionWithSpec);

            // Wrap the subpipeline's explain output in a format that can be parsed by
            // 'getAggPlanStages'.
            verifyEqLookupNodeStrategy({stages: unionWithSpec["pipeline"]}, 0, "NestedLoopJoin");
            assert(unionColl.drop());
        }());

MongoRunner.stopMongod(conn);

(function testHashJoinQueryKnobs() {
    // Create a new scope and start a new mongod so that the mongod-wide global state changes do not
    // affect subsequent tests if any.
    const conn = MongoRunner.runMongod({setParameter: "featureFlagSBELookupPushdown=true"});
    const db = conn.getDB(name);
    const lcoll = db.query_knobs_local;
    const fcoll = db.query_knobs_foreign;

    assert.commandWorked(lcoll.insert({a: 1}));
    assert.commandWorked(fcoll.insert([{a: 1}, {a: 1}]));

    // The foreign collection is very small and first verifies that the HJ is chosen under the
    // default query knob values.
    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // The fcollStats.count means the number of documents in a collection, the fcollStats.size means
    // the collection's data size, and the fcollStats.storageSize means the allocated storage size.
    const fcollStats = assert.commandWorked(fcoll.stats());
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size,
        internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin: fcollStats.storageSize
    }));

    // Verifies that the HJ is still chosen.
    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Setting the 'internalQueryDisableLookupExecutionUsingHashJoin' knob to true will disable
    // HJ plans from being chosen and since the pipeline is SBE compatible it will fallback to
    // NLJ.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryDisableLookupExecutionUsingHashJoin: true,
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Test that we can go back to generating HJ plans.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryDisableLookupExecutionUsingHashJoin: false,
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.HJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Setting the 'internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin' to count - 1 results in
    // choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Reverting back 'internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin' to the previous
    // value. Setting the 'internalQueryCollectionMaxDataSizeBytesToChooseHashJoin' to size - 1
    // results in choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin: fcollStats.count,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    // Reverting back 'internalQueryCollectionMaxDataSizeBytesToChooseHashJoin' to the previous
    // value. Setting the 'internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin' to
    // storageSize - 1 results in choosing the NLJ algorithm.
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryCollectionMaxDataSizeBytesToChooseHashJoin: fcollStats.size,
        internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin: fcollStats.storageSize - 1
    }));

    runTest(lcoll,
            [{$lookup: {from: fcoll.getName(), localField: "a", foreignField: "a", as: "out"}}],
            JoinAlgorithm.NLJ,
            null /* indexKeyPattern */,
            {allowDiskUse: true});

    MongoRunner.stopMongod(conn);
}());

// Sharded cases.
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    other: {
        shardOptions: {
            setParameter:
                {featureFlagSBELookupPushdown: true, featureFlagSBELookupPushdownIndexJoin: true}
        }
    }
});
db = st.s.getDB(name);

// Setup. Here, 'coll' is sharded, 'foreignColl' is unsharded, 'viewName' is an unsharded view,
// and 'shardedViewName' is a sharded view.
const shardedViewName = "sharded_foreign_view";
coll = db[name];
assert.commandWorked(coll.insert({a: 1, shardKey: 1}));
assert.commandWorked(coll.insert({a: 2, shardKey: 10}));
assert.commandWorked(coll.createIndex({shardKey: 1}));
st.shardColl(coll.getName(), {shardKey: 1}, {shardKey: 5}, {shardKey: 5}, name);

foreignColl = db[foreignCollName];
assert.commandWorked(foreignColl.insert({b: 5}));

assert.commandWorked(db.createView(viewName, foreignCollName, [{$match: {b: {$gte: 0}}}]));
assert.commandWorked(db.createView(shardedViewName, name, [{$match: {b: {$gte: 0}}}]));

(function testLookupPushdownAgainstShardedCluster() {
    // Both collections are unsharded.
    runTest(foreignColl,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.NLJ /* expectedJoinAlgorithm */);

    // Sharded main collection, unsharded right side. This is not expected to be eligible for
    // pushdown because the $lookup will be preceded by a $mergeCursors stage on the merging shard.
    runTest(coll,
            [{$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Sharded main collection, unsharded right side. Here, we are targeting a single shard, so
    // there will be no leading $mergeCursors stage. We should still avoid pushing down $lookup.
    const singleShardPipeline = [
        {$match: {shardKey: 1}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "out"}}
    ];
    runTest(coll, singleShardPipeline, JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Verify that the above pipeline targets a single shard and doesn't use a $mergeCursors stage.
    const singleShardExplain = coll.explain().aggregate(singleShardPipeline);
    assert(!aggPlanHasStage(singleShardExplain,
                            "$mergeCursors",
                            "found $mergeCursors in " + tojson(singleShardExplain)));
    assert(singleShardExplain.hasOwnProperty("shards"),
           "should have shards property in explain: " + tojson(singleShardExplain));
    assert.eq(Object.keys(singleShardExplain["shards"]).length,
              1,
              "sharded explain should only" +
                  " target one shard " + tojson(singleShardExplain));

    // Both collections are sharded.
    runTest(coll,
            [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Unsharded main collection, sharded right side.
    runTest(foreignColl,
            [{$lookup: {from: name, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Unsharded main collection, unsharded view right side.
    runTest(foreignColl,
            [{$lookup: {from: viewName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);

    // Unsharded main collection, sharded view on the right side.
    runTest(foreignColl,
            [{$lookup: {from: shardedViewName, localField: "a", foreignField: "b", as: "out"}}],
            JoinAlgorithm.Classic /* expectedJoinAlgorithm */);
}());
st.stop();
}());

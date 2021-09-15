/*
 * Tests that the 'changeStreamPreAndPostImages' option is settable via the collMod and create
 * commands. Also tests that this option cannot be set on collections in the 'local' or 'admin'
 * databases as well as timeseries and view collections.
 * @tags: [requires_fcv_51, featureFlagChangeStreamPreAndPostImages]
 */
(function() {
'use strict';

load("jstests/libs/collection_options.js");  // For assertCollectionOptionIsEnabled,
                                             // assertCollectionOptionIsAbsent.

const rsTest = new ReplSetTest({name: jsTestName(), nodes: 1});
rsTest.startSet();
rsTest.initiate();

const dbName = 'testDB';
const collName = 'changeStreamPreAndPostImages';
const collName2 = 'changeStreamPreAndPostImages2';
const collName3 = 'changeStreamPreAndPostImages3';
const collName4 = 'changeStreamPreAndPostImages4';
const viewName = "view";
const createTimeseriesOptions = {
    timeField: "a"
};

const primary = rsTest.getPrimary();
const adminDB = primary.getDB("admin");
const localDB = primary.getDB("local");
const configDB = primary.getDB("config");
const testDB = primary.getDB(dbName);

// Check that we cannot set 'changeStreamPreAndPostImages' on the local or admin databases.
for (const db of [localDB, adminDB, configDB]) {
    assert.commandFailedWithCode(
        db.runCommand({create: collName, changeStreamPreAndPostImages: true}),
        ErrorCodes.InvalidOptions);

    assert.commandWorked(db.runCommand({create: collName}));
    assert.commandFailedWithCode(
        db.runCommand({collMod: collName, changeStreamPreAndPostImages: true}),
        ErrorCodes.InvalidOptions);
}

// Should be able to set the 'changeStreamPreAndPostImages' via create or collMod.
assert.commandWorked(testDB.runCommand({create: collName, changeStreamPreAndPostImages: true}));
assertCollectionOptionIsEnabled(testDB, collName, "changeStreamPreAndPostImages");

assert.commandWorked(testDB.runCommand({create: collName2}));
assert.commandWorked(testDB.runCommand({collMod: collName2, changeStreamPreAndPostImages: true}));
assertCollectionOptionIsEnabled(testDB, collName2, "changeStreamPreAndPostImages");

// Verify that setting collection options with 'collMod' command does not affect
// 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand({"collMod": collName2, validationLevel: "off"}));
assertCollectionOptionIsEnabled(testDB, collName2, "changeStreamPreAndPostImages");

// Should successfully unset 'changeStreamPreAndPostImages' using the 'collMod' command.
assert.commandWorked(testDB.runCommand({collMod: collName2, changeStreamPreAndPostImages: false}));
assertCollectionOptionIsAbsent(testDB, collName2, "changeStreamPreAndPostImages");

// Both 'recordPreImages' and 'changeStreamPreAndPostImages' may not be set to true at the same
// time.
assert.commandFailedWithCode(
    testDB.runCommand(
        {create: collName3, recordPreImages: true, changeStreamPreAndPostImages: true}),
    ErrorCodes.InvalidOptions);

assert.commandWorked(testDB.runCommand({create: collName3}));
assert.commandFailedWithCode(
    testDB.runCommand(
        {collMod: collName3, recordPreImages: true, changeStreamPreAndPostImages: true}),
    ErrorCodes.InvalidOptions);

// Should set 'recordPreImages' to true and 'changeStreamPreAndPostImages' to false.
assert.commandWorked(testDB.runCommand(
    {collMod: collName3, recordPreImages: true, changeStreamPreAndPostImages: false}));
assertCollectionOptionIsAbsent(testDB, collName3, "changeStreamPreAndPostImages");
assertCollectionOptionIsEnabled(testDB, collName3, "recordPreImages");

// Should set 'recordPreImages' to false and 'changeStreamPreAndPostImages' to true.
assert.commandWorked(testDB.runCommand(
    {collMod: collName3, recordPreImages: false, changeStreamPreAndPostImages: true}));
assertCollectionOptionIsEnabled(testDB, collName3, "changeStreamPreAndPostImages");
assertCollectionOptionIsAbsent(testDB, collName3, "recordPreImages");

// Set 'recordPreImages: true' to disable 'changeStreamPreAndPostImages' option.
assert.commandWorked(testDB.runCommand({"collMod": collName3, "recordPreImages": true}));

// 'changeStreamPreAndPostImages' field must be absent and 'recordPreImages' should be set to
// true.
assertCollectionOptionIsEnabled(testDB, collName3, "recordPreImages");
assertCollectionOptionIsAbsent(testDB, collName3, "changeStreamPreAndPostImages");

// Enable pre-/post-images for the collection with 'changeStreamPreAndPostImages' enabled.
// Set 'changeStreamPreAndPostImages: true' to disable 'recordPreImages' option.
assert.commandWorked(
    testDB.runCommand({"collMod": collName3, "changeStreamPreAndPostImages": true}));

// 'changeStreamPreAndPostImages' field must be set to true and 'recordPreImages' should be
// absent.
assertCollectionOptionIsAbsent(testDB, collName3, "recordPreImages");
assertCollectionOptionIsEnabled(testDB, collName3, "changeStreamPreAndPostImages");

// Should fail to create a timeseries collection with 'changeStreamPreAndPostImages' set to true.
assert.commandFailedWithCode(testDB.runCommand({
    create: collName4,
    timeseries: createTimeseriesOptions,
    changeStreamPreAndPostImages: true
}),
                             ErrorCodes.InvalidOptions);

assert.commandWorked(testDB.runCommand({create: collName4, timeseries: createTimeseriesOptions}));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName4, changeStreamPreAndPostImages: true}),
    ErrorCodes.InvalidOptions);
assertCollectionOptionIsAbsent(testDB, collName4, "changeStreamPreAndPostImages");

// Should fail to create a view with 'changeStreamPreAndPostImages' set to true.
assert.commandFailedWithCode(
    testDB.runCommand({create: viewName, viewOn: collName, changeStreamPreAndPostImages: true}),
    ErrorCodes.InvalidOptions);
assert.commandWorked(testDB.runCommand({create: viewName, viewOn: collName}));
assert.commandFailedWithCode(
    testDB.runCommand({collMod: viewName, changeStreamPreAndPostImages: true}),
    ErrorCodes.InvalidOptions);

rsTest.stopSet();
}());

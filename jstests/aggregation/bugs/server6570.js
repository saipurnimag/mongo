// ensure $add asserts on string
load('jstests/aggregation/extras/utils.js');
load("jstests/libs/sbe_assert_error_override.js");  // Override error-code-checking APIs.

c = db.s6570;
c.drop();
c.save({x: 17, y: "foo"});

assertErrorCode(
    c, {$project: {string_fields: {$add: [3, "$y", 4, "$y"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {number_fields: {$add: ["a", "$x", "b", "$x"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {all_strings: {$add: ["c", "$y", "d", "$y"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {potpourri_1: {$add: [5, "$y", "e", "$x"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {potpourri_2: {$add: [6, "$x", "f", "$y"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {potpourri_3: {$add: ["g", "$y", 7, "$x"]}}}, ErrorCodes.TypeMismatch);
assertErrorCode(
    c, {$project: {potpourri_4: {$add: ["h", "$x", 8, "$y"]}}}, ErrorCodes.TypeMismatch);

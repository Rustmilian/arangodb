/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertFalse, assertEqual, assertNotEqual, fail */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for single operation nodes in cluster
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Wilfried Goesgens
/// @author Copyright 2018, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const internal = require("internal");
const errors = internal.errors;
const db = require("@arangodb").db;
const helper = require("@arangodb/aql-helper");

function optimizerClusterSingleDocumentTestSuite() {
  var ruleName = "optimize-cluster-single-document-operations";
  // various choices to control the optimizer:
  var thisRuleEnabled = {optimizer: {rules: ["+all", "-move-filters-into-enumerate", "-parallelize-gather", "-async-prefetch"]}}; // we can only work after other rules
  var thisRuleDisabled = {optimizer: {rules: ["+all", "-move-filters-into-enumerate", "-parallelize-gather", "-async-prefetch", "-" + ruleName]}};
  var notHereDoc = "notHereDoc";
  var yeOldeDoc = "yeOldeDoc";

  var cn1 = "UnitTestsCollection";
  var c1;

  var cn2 = "UnitTestsCollectionEmty";
  var c2;

  var cn3 = "UnitTestsCollectionModify";
  var c3;

  var s = function () {
  };

  var setupC1 = function () {
    db._drop(cn1);
    c1 = db._create(cn1, {numberOfShards: 5});

    for (var i = 0; i < 20; ++i) {
      c1.save({_key: `${i}`, group: "test" + (i % 10), value1: i, value2: i % 5});
    }
  };

  var setupC2 = function () {
    db._drop(cn2);
    c2 = db._create(cn2, {numberOfShards: 5});
    c2.save({_key: yeOldeDoc});
  };

  var setupC3 = function () {
    db._drop(cn3);
    c3 = db._create(cn3, {numberOfShards: 5});

    for (var i = 0; i < 20; ++i) {
      c3.save({_key: `${i}`, group: "test" + (i % 10), value1: i, value2: i % 5});
    }
  };

  var pruneRevisions = function (obj) {
    if (typeof obj instanceof Array) {
      obj.forEach(function (doc) {
        pruneRevisions(doc);
      });
    } else {
      if ((obj !== null) && (typeof obj !== "string")) {
        if (obj instanceof Object) {
          if (obj.hasOwnProperty('_rev')) {
            obj._rev = "wedontcare";
          }
          for (var property in obj) {
            if (!obj.hasOwnProperty(property)) continue;
            pruneRevisions(obj[property]);
          }
        }
      }
    }
  };

  var explain = function (result) {
    return helper.getCompactPlan(result).map(function (node) {
      return node.type;
    });
  };
  var runTestSet = function (sets, expectedRules, expectedNodes) {
    let count = 0;
    sets.forEach(function (set) {
      let queryString = set[0];
      let expectRule = expectedRules[set[1]];
      let expectNode = expectedNodes[set[2]];
      let doFullTest = set[3]; // Do advanced checking
      let setupFunction = set[4]; // this resets the setup
      let errorCode = set[5]; // expected error code
      let returnKey = set[6];
      let returnValue = set[7];
      const queryInfo = "count: " + count + " query info: " + JSON.stringify(set);

      var result = db._createStatement({query: queryString, bindVars:  {}, options:  thisRuleEnabled}).explain(); // explain - only
      assertEqual(expectRule, result.plan.rules, "rules mismatch: " + queryInfo);
      assertEqual(expectNode, explain(result), "nodes mismatch: " + queryInfo);
      if (doFullTest) {
        var r1 = {json: []}, r2 = {json: []};

        // run it first without the rule
        setupFunction();
        try {
          r1 = db._createStatement({query: queryString, bindVars:  {}, options:  thisRuleDisabled}).execute().toArray();
          assertEqual(0, errorCode, "we have no error in the original, but the tests expects an exception: " + queryInfo);
          if (typeof returnKey !== 'undefined') {
            assertEqual(r1[0][returnKey], returnValue);
          }
        } catch (y) {
          assertTrue(errorCode.hasOwnProperty('code'), "original plan throws, but we don't expect an exception" + JSON.stringify(y) + queryInfo);
          assertEqual(y.errorNum, errorCode.code, "match other error code - got: " + JSON.stringify(y) + queryInfo);
        }

        // Run it again with our rule
        setupFunction();
        try {
          r2 = db._createStatement({query: queryString, bindVars:  {}, options:  thisRuleEnabled}).execute().toArray();
          assertEqual(0, errorCode, "we have no error in our plan, but the tests expects an exception" + queryInfo);
          if (typeof returnKey !== 'undefined') {
            assertEqual(r1[0][returnKey], returnValue);
          }
        } catch (x) {
          assertTrue(errorCode.hasOwnProperty('code'), "our plan throws, but we don't expect an exception" + JSON.stringify(x) + queryInfo);
          assertEqual(x.errorNum, errorCode.code, "match our error code" + JSON.stringify(x) + queryInfo);
        }
        pruneRevisions(r1);
        pruneRevisions(r2);
        assertEqual(r1, r2, "results of with and without rule differ, queryInfo: " + queryInfo + ", set: " + set);
      }
      count += 1;
    });
  };

  let assertForRuleInQuery = function (queryInfo, res) {
    if (queryInfo.ruleToRemove.hasOwnProperty("optimizer")) {
      assertEqual(-1, res.plan.rules.indexOf(ruleName));
    } else {
      assertNotEqual(-1, res.plan.rules.indexOf(ruleName), "query does not trigger rule as expected");
    }
  };

  let pushStats = function (queryInfo, stats) {
    queryInfo.statsInfo.push({
      writesExecuted: stats.writesExecuted,
      writesIgnored: stats.writesIgnored,
      scannedIndex: stats.scannedIndex
    });
  };

  return {
    setUp: function () {
      setupC1();
      setupC2();
      setupC3();
    },

    tearDown: function () {
      db._drop(cn1);
      db._drop(cn2);
      db._drop(cn3);
    },

    testNumericKeyInsert: function () {
      try {
        db._query("INSERT { _key: 1234 } INTO " + cn1);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_DOCUMENT_KEY_BAD.code, err.errorNum);
      }
    },

    testNumericKeyUpdate: function () {
      try {
        db._query("UPDATE { _key: 1234 } WITH { value: 1 } IN " + cn1);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_DOCUMENT_KEY_MISSING.code, err.errorNum);
      }
    },

    testNumericKeyReplace: function () {
      try {
        db._query("REPLACE { _key: 1234 } WITH { value: 1 } IN " + cn1);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_DOCUMENT_KEY_MISSING.code, err.errorNum);
      }
    },

    testNumericKeyRemove: function () {
      try {
        db._query("REMOVE { _key: 1234 } IN " + cn1);
        fail();
      } catch (err) {
        assertEqual(errors.ERROR_ARANGO_DOCUMENT_KEY_MISSING.code, err.errorNum);
      }
    },

    testFetchDocumentWithOldAttribute: function () {
      let doc = c1.save({_key: "oldDoc", old: "abc"});

      const queries = [
        ["FOR one IN @@cn1 FILTER one._key == 'oldDoc' RETURN one._id", cn1 + "/oldDoc"],
        ["FOR one IN @@cn1 FILTER one._key == 'oldDoc' RETURN one._key", "oldDoc"],
        ["FOR one IN @@cn1 FILTER one._key == 'oldDoc' RETURN one._rev", doc._rev],
        ["FOR one IN @@cn1 FILTER one._key == 'oldDoc' RETURN one.old", "abc"],
        ["FOR one IN @@cn1 FILTER one._key == 'oldDoc' RETURN one", {
          _key: "oldDoc",
          _rev: doc._rev,
          old: "abc",
          _id: cn1 + "/oldDoc"
        }],
      ];

      queries.forEach(function (query) {
        let result = db._createStatement({query: query[0], bindVars:  {"@cn1": cn1}}).explain();
        assertNotEqual(-1, result.plan.rules.indexOf(ruleName));

        result = db._query(query[0], {"@cn1": cn1}).toArray();
        assertEqual(query[1], result[0]);
      });
    },

    testFetchDocumentWithNewAttribute: function () {
      let doc = c1.save({_key: "newDoc", "new": "abc"});

      const queries = [
        ["FOR one IN @@cn1 FILTER one._key == 'newDoc' RETURN one._id", cn1 + "/newDoc"],
        ["FOR one IN @@cn1 FILTER one._key == 'newDoc' RETURN one._key", "newDoc"],
        ["FOR one IN @@cn1 FILTER one._key == 'newDoc' RETURN one._rev", doc._rev],
        ["FOR one IN @@cn1 FILTER one._key == 'newDoc' RETURN one.`new`", "abc"],
        ["FOR one IN @@cn1 FILTER one._key == 'newDoc' RETURN one", {
          _key: "newDoc",
          _rev: doc._rev,
          "new": "abc",
          _id: cn1 + "/newDoc"
        }],
      ];

      queries.forEach(function (query) {
        let result = db._createStatement({query: query[0], bindVars:  {"@cn1": cn1}}).explain();
        assertNotEqual(-1, result.plan.rules.indexOf(ruleName));

        result = db._query(query[0], {"@cn1": cn1}).toArray();
        assertEqual(query[1], result[0]);
      });
    },

    ////////////////////////////////////////////////////////////////////////////////
    /// @brief test plans that should result
    ////////////////////////////////////////////////////////////////////////////////

    testRuleFetch: function () {
      var queries = [
        ["FOR d IN " + cn1 + " FILTER d._key == '1' RETURN d", 0, 0, true, s, 0],
        ["FOR d IN " + cn1 + " FILTER '1' == d._key RETURN d", 0, 0, true, s, 0],
        ["FOR d IN " + cn1 + " FILTER d.xyz == '1' RETURN d", 1, 1, false, s, 0],
        ["FOR d IN " + cn1 + " FILTER d._key == '1' RETURN 123", 2, 2, true, s, 0],
        ["FOR d IN " + cn1 + " FILTER d._key == '1' LIMIT 10, 1 RETURN d", 3, 3, false, s, 0],
        ["FOR d IN " + cn1 + " FILTER d._key == '1' RETURN d._key", 4, 4, true, s, 0],
      ];
      var expectedRules = [["use-indexes",
        "remove-filter-covered-by-index",
        "remove-unnecessary-calculations-2",
        "optimize-cluster-single-document-operations"],
        ["scatter-in-cluster",
          "distribute-filtercalc-to-cluster",
          "remove-unnecessary-remote-scatter"],
        ["move-calculations-up",
          "use-indexes",
          "remove-filter-covered-by-index",
          "remove-unnecessary-calculations-2",
          "optimize-cluster-single-document-operations"],
        ["use-indexes", "remove-filter-covered-by-index",
          "remove-unnecessary-calculations-2",
          "scatter-in-cluster", "remove-unnecessary-remote-scatter",
          "restrict-to-single-shard"],
        ["move-calculations-up",
          "move-filters-up",
          "move-calculations-up-2",
          "move-filters-up-2",
          "use-indexes",
          "remove-filter-covered-by-index",
          "remove-unnecessary-calculations-2",
          "optimize-cluster-single-document-operations"],
      ];

      var expectedNodes = [
        ["SingletonNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "EnumerateCollectionNode", "CalculationNode",
          "FilterNode", "RemoteNode", "GatherNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode",
          "ReturnNode"],
        ["SingletonNode", "IndexNode", "RemoteNode", "GatherNode", "LimitNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
      ];
      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleInsert: function () {
      var queries = [
        // [ query, expectedRulesField, expectedNodesField, doFullTest, setupFunction, errorCode ]
        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} OPTIONS {waitForSync: true, ignoreErrors:true}`, 0, 0, true, setupC2, 0],
        [`INSERT {_key: '${yeOldeDoc}',  insert1: true} IN ${cn2} OPTIONS {waitForSync: true, ignoreErrors:true}`, 0, 0, true, setupC2, 0],

        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN OLD`, 0, 1, true, setupC2, 0],
        [`INSERT {_key: '${yeOldeDoc}',  insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN OLD`, 0, 1, true, setupC2, 0],

        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN NEW`, 0, 1, true, setupC2, 0],
        [`INSERT {_key: '${yeOldeDoc}',  insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN NEW`, 0, 1, true, setupC2, 0],

        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: false} RETURN NEW`, 0, 1, true, setupC2, 0],
        [`INSERT {_key: '${yeOldeDoc}',  insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: false} RETURN NEW`, 0, 1, true, setupC2, errors.ERROR_ARANGO_UNIQUE_CONSTRAINT_VIOLATED],

        [`INSERT {_key: '${yeOldeDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN [OLD, NEW]`, 1, 2, true, setupC2, 0],
        [`INSERT {_key: '${yeOldeDoc}', insert1: true} IN   ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN { old: OLD, new: NEW }`, 1, 2, true, setupC2, 0],
        [`LET a = { a: 123 } INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN [OLD, NEW, a]`, 2, 2, true, setupC2, 0],
        [`LET a = { a: 123 } INSERT {_key: '${yeOldeDoc}',  insert1: true} IN ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN { old: OLD, new: NEW, a: a }`, 2, 2, true, setupC2, 0],
        [`LET a = { a: 123 } INSERT {_key: '${notHereDoc}', insert1: true} IN  ${cn2} OPTIONS {waitForSync: true, overwrite: true} RETURN a`, 3, 3, true, setupC2, 0],
        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} RETURN NEW`, 0, 1, true, setupC2, 0],
        [`INSERT {_key: '${notHereDoc}', insert1: true} IN ${cn2} RETURN NEW._key`, 0, 2, true, setupC2, 0],
      ];

      var expectedRules = [
        ["remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-unnecessary-calculations", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-redundant-calculations", "remove-unnecessary-calculations", "move-calculations-up-2",
          "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"]
      ];
      var expectedNodes = [
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode"]
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleUpdate: function () {
      var queries = [
        ["UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        ["UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        ["UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        ["UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        ["UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        ["UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        ["UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 3, 2, false],
        ["UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 3, 2, false],
        ["UPDATE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 3, 2, false],
        ["UPDATE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 3, 2, false],
        ["UPDATE {_key: '1', boom: true } INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 3, 2, true, setupC1, 0],
        ["UPDATE {_key: '1'} WITH {foo: 'bar1a'} IN " + cn1 + " OPTIONS {}", 1, 0, true, s, 0],
        ["UPDATE {_key: '1'} WITH {foo: 'bar2a'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        ["UPDATE {_key: '1'} WITH {foo: 'bar3a'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, s, 0],
        ["UPDATE {_key: '1'} WITH {foo: 'bar4a'} IN " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 2, 2, true, setupC1, 0],
        ["UPDATE {_key: '1'} WITH {foo: 'bar5a'} IN " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 2, 2, true, setupC1, 0],
        ["UPDATE {_key: '1'} INTO " + cn1 + " RETURN OLD._key", 0, 2, true, s, 0],
        ["UPDATE {_key: '1'} WITH {_key: '1', name: 'test1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0, "name", "test1"],
        ["UPDATE {_key: '1'} WITH {_key: '2'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0],
        ["UPDATE {_key: '1'} WITH {_key: '1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 12, 1, true, setupC1, 0],
        ["UPDATE '1' WITH {_key: '2'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        ["UPDATE '1' WITH {_key: '1'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        ["UPDATE '1' WITH {_key: '1', name: 'test1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0, "name", "test1"],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc INTO ${cn1} OPTIONS {} RETURN NEW`, 4, 3, true, s, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW]`, 8, 2, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 8, 1, true, setupC1, 0, "name", "test1"],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE '1' WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 10, 5, true, setupC1, 0, "name", "test1"],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE '1' WITH {_key: '1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 10, 5, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '${notHereDoc}' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW]`, 8, 2, true, setupC1, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE '1' WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 11, 5, true, setupC1, 0, "name", "test1"],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE '1' WITH {_key: '1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 11, 5, true, setupC1, 0],
        [`LET a = { a: 123 } FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc INTO ${cn1} OPTIONS {} RETURN { NEW: NEW, a: a }`, 6, 4, true, s, 0],
        [`LET a = { a: 123 } FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW, a]`, 9, 2, true, setupC1, 0],
        [`LET a = { a: 123 } FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc INTO ${cn1} OPTIONS {} RETURN [ NEW, a ]`, 6, 4, true, s, 0],
        [`LET a = { a: 123 } FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW, a]`, 9, 2, true, setupC1, 0],
        [`LET a = { a: 123 } FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [doc, NEW, a]`, 9, 2, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' UPDATE doc WITH {foo: 'bar'} INTO ${cn1} RETURN OLD._key`, 8, 2, true, setupC1, 0],
      ];

      var expectedRules = [
        ["remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "optimize-cluster-single-document-operations"],
        ["optimize-cluster-single-document-operations"],
        ["remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-unnecessary-calculations", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "distribute-in-cluster", "scatter-in-cluster", "remove-unnecessary-remote-scatter", "restrict-to-single-shard"],
        ["move-calculations-up", "remove-unnecessary-calculations", "move-calculations-up-2", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "distribute-in-cluster", "scatter-in-cluster", "remove-unnecessary-remote-scatter", "restrict-to-single-shard"],
        ["move-calculations-up", "remove-redundant-calculations", "remove-unnecessary-calculations", "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
      ];

      var expectedNodes = [
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "CalculationNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "DistributeNode", "RemoteNode", "UpdateNode", "RemoteNode", "GatherNode", "ReturnNode"]

      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleReplace: function () {
      var queries = [
        ["REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        ["REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, s, 0],
        ["REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        ["REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, s, 0],
        ["REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        ["REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN NEW", 0, 1, true, s, 0],
        ["REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 3, 2, false],
        ["REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 3, 2, false],
        ["REPLACE {_key: '1'} IN   " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 3, 2, false],
        ["REPLACE {_key: '1'} INTO " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 3, 2, false],
        ["REPLACE {_key: '1'} WITH {foo: 'bar1a'} IN " + cn1 + " OPTIONS {}", 1, 0, true, s, 0],
        ["REPLACE {_key: '1'} WITH {foo: 'bar2a'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        ["REPLACE {_key: '1'} WITH {foo: 'bar2a'} IN " + cn1 + " OPTIONS {} RETURN OLD._key", 10, 2, true, setupC1, 0],
        ["REPLACE {_key: '1'} WITH {foo: 'bar3a'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, s, 0],
        ["REPLACE {_key: '1'} WITH {foo: 'bar4a'} IN " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 2, 2, true, setupC1, 0],
        ["REPLACE {_key: '1', boom: true } IN   " + cn1 + " OPTIONS {} RETURN [OLD, NEW]", 3, 2, true, setupC1, 0],
        ["REPLACE {_key: '1'} WITH {foo: 'bar5a'} IN " + cn1 + " OPTIONS {} RETURN { old: OLD, new: NEW }", 2, 2, true, setupC1, 0],
        ["REPLACE {_key: '1'} WITH {_key: '1', name: 'test1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0, "name", "test1"],
        ["REPLACE {_key: '1'} WITH {_key: '2'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0],
        ["REPLACE {_key: '1'} WITH {_key: '1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 13, 1, true, setupC1, 0],
        ["REPLACE '1' WITH {_key: '1', name: 'test1'} IN " + cn1 + " OPTIONS {} RETURN NEW", 1, 1, true, setupC1, 0, "name", "test1"],
        ["REPLACE '1' WITH {_key: '2'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],
        ["REPLACE '1' WITH {_key: '1'} IN " + cn1 + " OPTIONS {} RETURN OLD", 1, 1, true, setupC1, 0],

        [`FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW]`, 8, 2, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE doc INTO ${cn1} OPTIONS {} RETURN NEW`, 4, 3, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE doc WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 8, 1, true, setupC1, 0, "name", "test1"],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE '1' WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 11, 5, true, setupC1, 0, "name", "test1"],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE '1' WITH {_key: '1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 11, 5, true, setupC1, 0],

        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE '1' WITH {_key: '1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 12, 5, true, setupC1, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '1' REPLACE '1' WITH {_key: '1', name: 'test1'} INTO ${cn1} OPTIONS {} RETURN NEW`, 12, 5, true, setupC1, 0, "name", "test1"],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '-1' REPLACE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW, a]`, 9, 2, true, setupC1, 0],

        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key ==  '1' REPLACE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [OLD, NEW, a]`, 9, 2, true, setupC1, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key ==  '1' REPLACE doc WITH {foo: 'bar'} INTO ${cn1} OPTIONS {} RETURN [doc, NEW, a]`, 9, 2, true, setupC1, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key ==  '1' REPLACE doc INTO ${cn1} OPTIONS {} RETURN [ NEW, a ]`, 7, 4, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key ==  '1' REPLACE doc INTO ${cn1} RETURN OLD._key`, 4, 4, true, setupC1, 0],
      ];

      var expectedRules = [
        ["remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "optimize-cluster-single-document-operations"],
        ["optimize-cluster-single-document-operations"],
        ["remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-unnecessary-calculations", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "move-calculations-up-2", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "distribute-in-cluster", "scatter-in-cluster", "remove-unnecessary-remote-scatter", "restrict-to-single-shard"],
        ["move-calculations-up", "remove-unnecessary-calculations", "move-calculations-up-2", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "distribute-in-cluster", "scatter-in-cluster", "remove-unnecessary-remote-scatter", "restrict-to-single-shard"],
        ["move-calculations-up", "remove-redundant-calculations", "remove-unnecessary-calculations", "remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
      ];

      var expectedNodes = [
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "CalculationNode", "IndexNode", "RemoteNode", "GatherNode", "CalculationNode", "DistributeNode", "RemoteNode", "ReplaceNode", "RemoteNode", "GatherNode", "ReturnNode"],
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testRuleRemove: function () {
      var queries = [
        ["REMOVE {_key: '1'} IN   " + cn1 + " OPTIONS {}", 0, 0, true, setupC1, 0],
        ["REMOVE {_key: '2'} INTO " + cn1 + " OPTIONS {}", 0, 0, true, setupC1, 0],
        ["REMOVE {_key: '3'} IN   " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, setupC1, 0],
        ["REMOVE {_key: '4'} INTO " + cn1 + " OPTIONS {} RETURN OLD", 0, 1, true, setupC1, 0],
        ["REMOVE {_key: '4'} INTO " + cn1 + " RETURN OLD._key", 0, 2, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == '1' REMOVE doc IN ${cn1} RETURN OLD`, 1, 1, true, setupC1, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN OLD`, 1, 1, true, s, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == '1' REMOVE doc IN ${cn1} RETURN [OLD, a]`, 2, 2, true, setupC1, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN [OLD, a]`, 2, 2, true, s, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN a`, 3, 3, true, s, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN doc`, 4, 1, true, s, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN OLD`, 2, 1, true, s, 0],
        [`LET a = 123 FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN OLD._key`, 4, 2, true, s, 0],
        [`FOR doc IN ${cn1} FILTER doc._key == 'notheredoc' REMOVE doc IN ${cn1} RETURN OLD._key`, 1, 2, true, s, 0],
      ];
      var expectedRules = [
        ["remove-data-modification-out-variables", "optimize-cluster-single-document-operations"],
        ["remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["move-calculations-up", "remove-redundant-calculations", "remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
        ["remove-unnecessary-calculations", "remove-data-modification-out-variables", "use-indexes", "remove-filter-covered-by-index", "remove-unnecessary-calculations-2", "optimize-cluster-single-document-operations"],
      ];

      var expectedNodes = [
        ["SingletonNode", "SingleRemoteOperationNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "ReturnNode"],
        ["SingletonNode", "SingleRemoteOperationNode", "CalculationNode", "ReturnNode"],
        ["SingletonNode", "CalculationNode", "SingleRemoteOperationNode", "ReturnNode"],
      ];

      runTestSet(queries, expectedRules, expectedNodes);
    },

    testSelectAndInsert: function () {
      let result = db._createStatement({query: "FOR one IN @@cn1 FILTER one._key == 'a' INSERT one INTO @@cn2", bindVars: {
        "@cn1": cn1,
        "@cn2": cn2
      }}).explain();
      assertEqual(-1, result.plan.rules.indexOf(ruleName));
    },

    testSimpleQueriesNotEligible: function () {
      let queries = [
        "FOR one IN @@cn1 FILTER one.abc == 'a' RETURN one",
        "FOR one IN @@cn1 FILTER one._key == 'a' UPDATE 'foo' WITH { two: 1 } IN @@cn1",
        "FOR one IN @@cn1 FILTER one._key == 'a' UPDATE 'foo' WITH { two: 1 } IN " + cn2,
        "FOR one IN @@cn1 FILTER one._key == 'a' UPDATE one WITH { two: 1 } IN " + cn2,
        "FOR one IN @@cn1 FILTER one._key == 'a' REPLACE 'foo' WITH { two: 1 } IN @@cn1",
        "FOR one IN @@cn1 FILTER one._key == 'a' REPLACE 'foo' WITH { two: 1 } IN " + cn2,
        "FOR one IN @@cn1 FILTER one._key == 'a' REPLACE one WITH { two: 1 } IN " + cn2,
        "FOR one IN @@cn1 FILTER one._key == 'a' REMOVE 'foo' IN @@cn1",
        "FOR one IN @@cn1 FILTER one._key == 'a' REMOVE 'foo' IN " + cn2,
        "FOR one IN @@cn1 FILTER one._key == 'a' REMOVE one IN " + cn2,

        "INSERT [] INTO @@cn1",
        "INSERT [ {} ] INTO @@cn1",
        "INSERT [{ foo: 1 }] INTO @@cn1",
        "INSERT [] INTO @@cn1 RETURN NEW",
        "INSERT [ {} ] INTO @@cn1 RETURN NEW",
        "INSERT [{ foo: 2 }] INTO @@cn1 RETURN NEW",
        "INSERT {} INTO @@cn1 OPTIONS { exclusive: true }",

        "UPDATE [] IN @@cn1",
        "UPDATE [ {} ] IN @@cn1",
        "UPDATE [{ foo: 3 }] IN @@cn1",
        "UPDATE [] IN @@cn1 RETURN OLD",
        "UPDATE [ {} ] IN @@cn1 RETURN OLD",
        "UPDATE [{ foo: 4 }] IN @@cn1 RETURN OLD",
        "UPDATE 'foo' WITH { two: 1 } INTO @@cn1 OPTIONS { exclusive: true }",

        "REPLACE [] IN @@cn1",
        "REPLACE [ {} ] IN @@cn1",
        "REPLACE [{ foo: 3 }] IN @@cn1",
        "REPLACE [] IN @@cn1 RETURN OLD",
        "REPLACE [ {} ] IN @@cn1 RETURN OLD",
        "REPLACE [{ foo: 4 }] IN @@cn1 RETURN OLD",
        "REPLACE { _key: 'abc' } WITH { two: 1 } INTO @@cn1 OPTIONS { exclusive: true }",

        "REMOVE [] IN @@cn1",
        "REMOVE [ {} ] IN @@cn1",
        "REMOVE [{ foo: 3 }] IN @@cn1",
        "REMOVE [] IN @@cn1 RETURN OLD",
        "REMOVE [ {} ] IN @@cn1 RETURN OLD",
        "REMOVE [{ foo: 4 }] IN @@cn1 RETURN OLD",
        "REMOVE { _key: 'abc' } INTO @@cn1 OPTIONS { exclusive: true }",
      ];

      queries.forEach(function (query) {
        let result = db._createStatement({query: query, bindVars:  {"@cn1": cn1}}).explain();
        assertEqual(-1, result.plan.rules.indexOf(ruleName), query);
      });
    },

    testSimpleQueriesEligible: function () {
      let queries = [
        "FOR one IN @@cn1 FILTER one._key == 'a' RETURN one",
        "FOR one IN @@cn1 FILTER one._key == 'a' UPDATE one WITH { two: 1 } IN @@cn1",
        "FOR one IN @@cn1 FILTER one._key == 'a' REPLACE one WITH { two: 1 } IN @@cn1",
        "FOR one IN @@cn1 FILTER one._key == 'a' REMOVE one IN @@cn1",
        "INSERT {} INTO @@cn1",
        "INSERT {} INTO @@cn1 OPTIONS { exclusive: false }",
        "INSERT { _key: 'abc' } INTO @@cn1",
        "INSERT { two: 1 } INTO @@cn1",
        "UPDATE 'foo' WITH { two: 1 } INTO @@cn1",
        "UPDATE 'foo' WITH { two: 1 } INTO @@cn1 OPTIONS { exclusive: false }",
        "UPDATE { _key: 'abc' } WITH { two: 1 } INTO @@cn1",
        "REPLACE { _key: 'abc' } WITH { two: 1 } INTO @@cn1",
        "REPLACE { _key: 'abc' } WITH { two: 1 } INTO @@cn1 OPTIONS { exclusive: false }",
        "REMOVE 'abc' INTO @@cn1",
        "REMOVE { _key: 'abc' } INTO @@cn1",
        "REMOVE { _key: 'abc' } INTO @@cn1 OPTIONS { exclusive: false }",
      ];

      queries.forEach(function (query) {
        let result = db._createStatement({query: query, bindVars:  {"@cn1": cn1}}).explain();
        assertNotEqual(-1, result.plan.rules.indexOf(ruleName), query);
      });
    },

    testStatsWhenApplyingRuleOneAtATimeCompare: function () {
      const collName = "testCollectionStats";
      const doc = {value1: 1, value2: "a"};
      const values = [1, 3];
      // to compare the query stats with and without the rule
      const queryInfo = [{
        ruleToRemove: {},
        statsInfo: []
      }, {ruleToRemove: {optimizer: {rules: ["-optimize-cluster-single-document-operations"]}}, statsInfo: []}];
      values.forEach(numShards => {
        queryInfo.forEach(queryInfo => {
          try {
            db._create(collName, {numberOfShards: numShards});
            let query = `INSERT @doc IN ${collName} OPTIONS {ignoreErrors: true} RETURN NEW`;
            let res = db._createStatement({query: query, bindVars:  {doc: doc}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {doc: doc}, queryInfo.ruleToRemove);
            let newDoc = res.toArray()[0];
            const key = newDoc["_key"];
            assertEqual(newDoc.value1, 1);
            assertEqual(newDoc.value2, "a");
            let stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `INSERT {_key: ${key}} IN ${collName} OPTIONS {ignoreErrors: true} RETURN NEW`;
            res = db._createStatement({query: query, bindVars:  {}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {}, queryInfo.ruleToRemove);
            assertEqual(res.toArray().length, 0);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `UPDATE {_key: @key} WITH {value1: 2, value2: 'abc'} IN ${collName} RETURN {old: OLD, new: NEW}`;
            res = db._createStatement({query: query, bindVars:  {key: key}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {key: key}, queryInfo.ruleToRemove);
            let result = res.toArray()[0];
            assertEqual(result.old.value1, 1);
            assertEqual(result.old.value2, "a");
            assertEqual(result.new.value1, 2);
            assertEqual(result.new.value2, "abc");
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `FOR d IN ${collName} FILTER d._key == @key RETURN d`;
            res = db._createStatement({query: query, bindVars:  {key: key}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {key: key}, queryInfo.ruleToRemove);
            result = res.toArray()[0];
            assertEqual(result["_key"], key);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `UPDATE {_key: "test1"} WITH {value1: 2, value2: 'abc'} IN ${collName} OPTIONS {ignoreErrors: true} RETURN {old: OLD, new: NEW}`;
            res = db._createStatement({query: query, bindVars:  {}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {}, queryInfo.ruleToRemove);
            assertEqual(res.toArray().length, 0);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `REPLACE {_key: @key} WITH {value1: 2} IN ${collName} RETURN {old: OLD, new: NEW}`;
            res = db._createStatement({query: query, bindVars:  {key: key}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {key: key}, queryInfo.ruleToRemove);
            result = res.toArray()[0];
            assertEqual(result.old.value1, 2);
            assertEqual(result.old.value2, "abc");
            assertFalse(result.new.hasOwnProperty("value2"));
            assertEqual(result.new.value1, 2);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `REPLACE {_key: "test1"} WITH {value1: 2} IN ${collName} OPTIONS {ignoreErrors: true} RETURN {old: OLD, new: NEW}`;
            res = db._createStatement({query: query, bindVars:  {}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {}, queryInfo.ruleToRemove);
            assertEqual(res.toArray().length, 0);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `REMOVE {_key: @key} IN ${collName}`;
            res = db._createStatement({query: query, bindVars:  {key: key}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {key: key}, queryInfo.ruleToRemove);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);

            query = `REMOVE {_key: @key} IN ${collName} OPTIONS {ignoreErrors: true} `;
            res = db._createStatement({query: query, bindVars:  {key: key}, options:  queryInfo.ruleToRemove}).explain();
            assertForRuleInQuery(queryInfo, res);
            res = db._query(query, {key: key}, queryInfo.ruleToRemove);
            stats = res.getExtra().stats;
            pushStats(queryInfo, stats);
            assertEqual(db[collName].count(), 0);
          } finally {
            db._drop(collName);
          }
        });
        assertEqual(queryInfo[0].statsInfo.length, queryInfo[1].statsInfo.length);
        queryInfo[0].statsInfo.forEach((info, idx) => {
          const infoWithoutRule = queryInfo[1].statsInfo;
          assertEqual(info.writesExecuted, infoWithoutRule[idx].writesExecuted);
          assertEqual(info.writesIgnored, infoWithoutRule[idx].writesIgnored);
          assertEqual(info.scannedIndex, infoWithoutRule[idx].scannedIndex);
        });
      });
    },
  };
}

jsunity.run(optimizerClusterSingleDocumentTestSuite);
return jsunity.done();

/*jshint globalstrict:false, strict:false, maxlen: 500 */
/*global assertTrue, assertFalse, assertEqual, assertNotEqual */

////////////////////////////////////////////////////////////////////////////////
/// @brief tests for sparse index usage
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
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

const jsunity = require("jsunity");
const db = require("@arangodb").db;
const normalize = require("@arangodb/aql-helper").normalizeProjections;

function optimizerSparseTestSuite () {
  let c;
  const opt = { optimizer: { rules: ["-move-filters-into-enumerate"] } };

  return {
    setUpAll : function () {
      db._drop("UnitTestsCollection");
      c = db._create("UnitTestsCollection", { numberOfShards: 5 });

      let docs = [];
      for (let i = 0; i < 2000; ++i) {
        docs.push({ _key: "test" + i, value1: i });
      }
      c.insert(docs);
    },

    tearDownAll : function () {
      db._drop("UnitTestsCollection");
    },

    testSparseHashEq : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseHashEqNull : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(0, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseHashNeNull : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseHashEqFunc : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseHashEqFuncNeNull : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseHashEqFuncGtNull : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 > null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseHashEqFuncGeNull : function () {
      c.ensureIndex({ type: "hash", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 >= null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistEq : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGt : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 > 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1989, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGe : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 >= 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1990, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistLt : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 < 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(10, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistLe : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 <= 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(11, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistLeNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 <= 10 && doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(11, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistLtNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 < 10 && doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(10, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGeNullRange : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 >= null && doc.value1 <= 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(11, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistGeNullRangeNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 >= null && doc.value1 <= 10 && doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(11, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGtNullRange : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 > null && doc.value1 <= 10 RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(11, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistEqNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(0, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGtNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 > null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistGeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 >= null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistEqFunc : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseSkiplistEqFuncNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 != null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistEqFuncGtNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 > null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertNotEqual(-1, index);
      assertTrue(nodes[index].indexes[0].sparse);
    },
    
    testSparseSkiplistEqFuncGeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 == NOOPT(10) && doc.value1 >= null RETURN doc";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let nodeTypes = nodes.map(function(n) { return n.type; });
      let index = nodeTypes.indexOf("IndexNode");
      assertEqual(-1, index);
    },
    
    testSparseJoin : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });

      let query = `
        FOR doc1 IN ${c.name()}
          FOR doc2 IN ${c.name()}
            FILTER doc1.value1 == 10
            FILTER doc1.value1 == doc2.value1
            RETURN doc1
      `;
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(2, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[1].indexes[0].sparse);
    },
    
    testSparseJoinFunc : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let opt = { optimizer: { rules: ["-interchange-adjacent-enumerations"] } };
      let query = "FOR doc1 IN " + c.name() + " FOR doc2 IN " + c.name() + " FILTER doc1.value1 == NOOPT(10) FILTER doc1.value1 == doc2.value1 RETURN doc1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement({query, bindVars: null, options: opt}).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(0, indexes.length);
      let collections = nodes.filter(function(n) { return n.type === 'EnumerateCollectionNode'; });
      assertEqual(2, collections.length);
      assertEqual(c.name(), collections[0].collection);
      assertEqual("doc1", collections[0].outVariable.name);
      assertEqual([], collections[0].projections);
      assertEqual(c.name(), collections[1].collection);
      assertEqual("doc2", collections[1].outVariable.name);
      assertEqual(normalize(["value1"]), normalize(collections[1].projections));
    },
    
    testSparseJoinFuncNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let opt = { optimizer: { rules: ["-interchange-adjacent-enumerations"] } };
      let query = "FOR doc1 IN " + c.name() + " FOR doc2 IN " + c.name() + " FILTER doc1.value1 == NOOPT(10) FILTER doc1.value1 == doc2.value1 FILTER doc1.value1 != null RETURN doc1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query, null, opt).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertEqual(c.name(), indexes[0].collection);
      assertEqual("doc1", indexes[0].outVariable.name);
      assertNotEqual({}, indexes[0].condition);
      assertTrue(indexes[0].indexes[0].sparse);
      
      let collections = nodes.filter(function(n) { return n.type === 'EnumerateCollectionNode'; });
      assertEqual(1, collections.length);
      assertEqual(c.name(), collections[0].collection);
      assertEqual("doc2", collections[0].outVariable.name);
      assertEqual(normalize(["value1"]), normalize(collections[0].projections));
    },
    
    testSparseJoinFuncNeNullNeNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });

      let query = `
        FOR doc1 IN ${c.name()}
          FOR doc2 IN ${c.name()}
            FILTER doc1.value1 == NOOPT(10)
            FILTER doc1.value1 == doc2.value1
            FILTER doc1.value1 != null
            FILTER doc2.value1 != null
            RETURN doc1`
      ;

      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(2, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[1].indexes[0].sparse);
    },
    
    testSparseJoinFuncGtNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let opt = { optimizer: { rules: ["-interchange-adjacent-enumerations"] } };
      let query = "FOR doc1 IN " + c.name() + " FOR doc2 IN " + c.name() + " FILTER doc1.value1 == NOOPT(10) FILTER doc1.value1 == doc2.value1 FILTER doc1.value1 > null RETURN doc1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query, null, opt).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertEqual(c.name(), indexes[0].collection);
      assertEqual("doc1", indexes[0].outVariable.name);
      assertNotEqual({}, indexes[0].condition);
      assertTrue(indexes[0].indexes[0].sparse);

      let collections = nodes.filter(function(n) { return n.type === 'EnumerateCollectionNode'; });
      assertEqual(1, collections.length);
      assertEqual(c.name(), collections[0].collection);
      assertEqual("doc2", collections[0].outVariable.name);
      assertEqual(normalize(["value1"]), normalize(collections[0].projections));
    },
    
    testSparseJoinFuncGtNullGtNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      
      let query = "FOR doc1 IN " + c.name() + " FOR doc2 IN " + c.name() + " FILTER doc1.value1 == NOOPT(10) FILTER doc1.value1 == doc2.value1 FILTER doc1.value1 > null FILTER doc2.value1 > null RETURN doc1";
      let results = db._query(query).toArray();
      assertEqual(1, results.length);

      let nodes = db._createStatement(query).explain().plan.nodes;
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(2, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[1].indexes[0].sparse);
    },
    
    testSparseConditionRemovalGreaterThan : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 > 1995 SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual([ 1996, 1997, 1998, 1999 ], results);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, and no extra filter!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(0, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
    },
    
    testSparseConditionRemovalNotNull : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, and no extra filter!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(0, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
    },
    
    testSparseConditionRemovalNotNullReverse : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null SORT doc.value1 DESC RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, and no extra filter!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertFalse(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(0, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
    },
    
    testSparseConditionRemovalCorrectCondition1 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertNotEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectConditionNoMoveFilters1 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement({query, bindVars: null, options: opt}).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(1, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectCondition2 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      c.ensureIndex({ type: "skiplist", fields: ["value1.foobar"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertNotEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectConditionNoMoveFilters2 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
      c.ensureIndex({ type: "skiplist", fields: ["value1.foobar"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement({query, bindVars: null, options: opt}).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(1, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectCondition3 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc._key != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(2000, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, no post-filter!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertNotEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectConditionNoMoveFilters3 : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc._key != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(2000, results.length);

      let plan = db._createStatement({query, bindVars: null, options: opt}).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, no post-filter!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(1, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectConditionDescending : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement(query).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertNotEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },
    
    testSparseConditionRemovalCorrectConditionDescendingNoMoveFilters : function () {
      c.ensureIndex({ type: "skiplist", fields: ["value1"], sparse: true });
        
      let query = "FOR doc IN " + c.name() + " FILTER doc.value1 != null FILTER doc.value1.foobar != null SORT doc.value1 RETURN doc.value1";
      let results = db._query(query, null, opt).toArray();
      assertEqual(0, results.length);

      let plan = db._createStatement({query, bindVars: null, options: opt}).explain().plan;
      let nodes = plan.nodes;
      // should use the sparse index, no sorting, but still post-filter on value1.foobar!
      let indexes = nodes.filter(function(n) { return n.type === 'IndexNode'; });
      assertEqual(1, indexes.length);
      assertTrue(indexes[0].indexes[0].sparse);
      assertTrue(indexes[0].ascending);
      
      let sorts = nodes.filter(function(n) { return n.type === 'SortNode'; });
      assertEqual(0, sorts.length);
      
      let filters = nodes.filter(function(n) { return n.type === 'FilterNode'; });
      assertEqual(1, filters.length);

      assertNotEqual(-1, plan.rules.indexOf("use-indexes"));
      assertNotEqual(-1, plan.rules.indexOf("use-index-for-sort"));
      assertNotEqual(-1, plan.rules.indexOf("remove-filter-covered-by-index"));
      assertEqual(-1, plan.rules.indexOf("move-filters-into-enumerate"));
    },

  };
}

jsunity.run(optimizerSparseTestSuite);

return jsunity.done();

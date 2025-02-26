/* jshint globalstrict:false, strict:false, unused : false */
/* global assertEqual */

// //////////////////////////////////////////////////////////////////////////////
// / @brief tests for dump/reload
// /
// / @file
// /
// / DISCLAIMER
// /
// / Copyright 2010-2012 triagens GmbH, Cologne, Germany
// /
// / Licensed under the Apache License, Version 2.0 (the "License")
// / you may not use this file except in compliance with the License.
// / You may obtain a copy of the License at
// /
// /     http://www.apache.org/licenses/LICENSE-2.0
// /
// / Unless required by applicable law or agreed to in writing, software
// / distributed under the License is distributed on an "AS IS" BASIS,
// / WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// / See the License for the specific language governing permissions and
// / limitations under the License.
// /
// / Copyright holder is triAGENS GmbH, Cologne, Germany
// /
// / @author Jan Steemann
// / @author Copyright 2012, triAGENS GmbH, Cologne, Germany
// //////////////////////////////////////////////////////////////////////////////

var db = require('@arangodb').db;
var internal = require('internal');
var jsunity = require('jsunity');

function runSetup () {
  'use strict';
  internal.debugClearFailAt();
  for (let i = 0; i < 5; ++i) {
    db._drop('UnitTestsRecovery' + i);
    let c = db._create('UnitTestsRecovery' + i);
    c.save({ _key: 'foo', value1: 'foo', value2: 'bar' });

    c.ensureIndex({ type: "persistent", fields: ["value1"] });
    c.ensureIndex({ type: "persistent", fields: ["value2"] });
  }

  // drop all indexes but primary
  let c;
  for (let i = 0; i < 4; ++i) {
    c = db._collection('UnitTestsRecovery' + i);
    let idx = c.getIndexes();
    for (let j = 1; j < idx.length; ++j) {
      c.dropIndex(idx[j].id);
    }
  }

  c.save({ _key: 'crashme' }, true);

}

// //////////////////////////////////////////////////////////////////////////////
// / @brief test suite
// //////////////////////////////////////////////////////////////////////////////

function recoverySuite () {
  'use strict';
  jsunity.jsUnity.attachAssertions();

  return {


    // //////////////////////////////////////////////////////////////////////////////
    // / @brief test whether the data are correct after a restart
    // //////////////////////////////////////////////////////////////////////////////

    testDropIndexes: function () {
      var i, c, idx;

      for (i = 0; i < 4; ++i) {
        c = db._collection('UnitTestsRecovery' + i);
        idx = c.getIndexes();
        assertEqual(1, idx.length);
        assertEqual('primary', idx[0].type);
      }

      c = db._collection('UnitTestsRecovery4');
      idx = c.getIndexes();
      assertEqual(3, idx.length);
    }

  };
}

// //////////////////////////////////////////////////////////////////////////////
// / @brief executes the test suite
// //////////////////////////////////////////////////////////////////////////////

function main (argv) {
  'use strict';
  if (argv[1] === 'setup') {
    runSetup();
    return 0;
  } else {
    jsunity.run(recoverySuite);
    return jsunity.writeDone().status ? 0 : 1;
  }
}

/* Copyright (C) 2012 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string>
#include "harness.h"

#include "common/anticache.h"

using namespace std;
using namespace voltdb;
using stupidunit::ChTempDir;

/**
 * This is just a simple test to prove that we can successfully
 * link in BerkeleyDB and include all of the headers that we need
 */
class AntiCacheTest : public Test {
public:
    AntiCacheTest() {
        
    };
};

TEST_F(AntiCacheTest, NextBlockId) {
    ChTempDir tempdir;
    AntiCacheDB anticache(NULL, ".");
    
    uint16_t lastBlockId;
    for (int i = 0; i < 1000; i++) {
        uint16_t blockId = anticache.nextBlockId();
        if (i > 0) ASSERT_NE(lastBlockId, blockId);
        lastBlockId = blockId;
    } // FOR
}

// This is based off of the code from Yi Wang
// http://cxwangyi.wordpress.com/2010/10/10/how-to-use-berkeley-db/
TEST_F(AntiCacheTest, WriteBlock) {
    // This will create a tempdir that will automatically be cleaned up
    ChTempDir tempdir;
    AntiCacheDB anticache(NULL, ".");

    string payload("Squirrels and Girls!");
    uint16_t blockId = anticache.nextBlockId();

    try {
        anticache.writeBlock(blockId,
                             const_cast<char*>(payload.data()),
                             static_cast<int>(payload.size())+1);
    } catch (...) {
        ASSERT_TRUE(false);
    }
}
int main() {
    return TestSuite::globalInstance()->runAll();
}

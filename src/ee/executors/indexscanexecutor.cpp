/* This file is part of VoltDB.
 * Copyright (C) 2008-2010 VoltDB L.L.C.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB L.L.C. are licensed under the following
 * terms and conditions:
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
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

#include "indexscanexecutor.h"

#include "common/debuglog.h"
#include "common/common.h"
#include "common/tabletuple.h"
#include "common/FatalException.hpp"
#include "expressions/abstractexpression.h"
#include "expressions/expressions.h"
#include "expressions/expressionutil.h"

// Inline PlanNodes
#include "plannodes/indexscannode.h"
#include "plannodes/projectionnode.h"
#include "plannodes/aggregatenode.h"
#include "plannodes/distinctnode.h"
#include "plannodes/limitnode.h"

#include "storage/table.h"
#include "storage/tableiterator.h"
#include "storage/tablefactory.h"
#include "storage/temptable.h"
#include "storage/persistenttable.h"

#ifdef ANTICACHE
#include "anticache/AntiCacheEvictionManager.h"
#endif

using namespace voltdb;

bool IndexScanExecutor::p_init(AbstractPlanNode *abstractNode,
                               const catalog::Database* catalogDb, int* tempTableMemoryInBytes)
{
    VOLT_TRACE("init IndexScan Executor");

    m_projectionNode = NULL;
    m_aggregateNode = NULL;
    m_distinctNode = NULL;
    m_limitNode = NULL;

    m_node = dynamic_cast<IndexScanPlanNode*>(abstractNode);
    assert(m_node);
    //
    // Now create our temp table where we can store our results
    // For now we are always use all of the columns, but in the future we may
    // want to have a projection work right inside of the SeqScan
    //
    assert(m_node->getTargetTable());

    //
    // INLINE PROJECTION
    //
    if (m_node->getInlinePlanNode(PLAN_NODE_TYPE_PROJECTION) != NULL)
    {
        m_projectionNode =
            static_cast<ProjectionPlanNode*>
            (m_node->getInlinePlanNode(PLAN_NODE_TYPE_PROJECTION));
        // XXX this assertion is useless if the above is a static_cast
        // assert(m_projectionNode);
        //
        // The internal node will already be initialized for us We
        // just need to use the internal node's output table which has
        // been formatted correctly based on the projection
        // information as our own output table
        assert(m_projectionNode->getOutputTable());
        m_node->setOutputTable(m_projectionNode->getOutputTable());

        m_projectionExpressions =
            new AbstractExpression*[m_node->getOutputTable()->columnCount()];

        ::memset(m_projectionExpressions, 0,
                 (sizeof(AbstractExpression*) *
                  m_node->getOutputTable()->columnCount()));

        m_projectionAllTupleArrayPtr =
          expressionutil::
            convertIfAllTupleValues(m_projectionNode->
                                    getOutputColumnExpressions());

        m_projectionAllTupleArray = m_projectionAllTupleArrayPtr.get();

        m_needsSubstituteProjectPtr =
            boost::shared_array<bool>
            (new bool[m_node->getOutputTable()->columnCount()]);
        m_needsSubstituteProject = m_needsSubstituteProjectPtr.get();

        for (int ctr = 0;
             ctr < m_node->getOutputTable()->columnCount();
             ctr++)
        {
            assert(m_projectionNode->getOutputColumnExpressions()[ctr]);
            m_needsSubstituteProjectPtr[ctr] =
              m_projectionNode->
                getOutputColumnExpressions()[ctr]->hasParameter();
            m_projectionExpressions[ctr] =
              m_projectionNode->getOutputColumnExpressions()[ctr];
        }
    //
    // FULL TABLE SCHEMA
    //
    }
    else
    {
        m_node->
            setOutputTable(TableFactory::
                           getCopiedTempTable(m_node->databaseId(),
                                              m_node->getTargetTable()->name() + " index",
                                              m_node->getTargetTable(),
                                              tempTableMemoryInBytes));
    }

    //
    // INLINE DISTINCT
    //
    if (m_node->getInlinePlanNode(PLAN_NODE_TYPE_DISTINCT) != NULL)
    {
        m_distinctNode =
            static_cast<DistinctPlanNode*>
            (m_node->getInlinePlanNode(PLAN_NODE_TYPE_DISTINCT));
        // XXX again, this assert is pointless if static_cast is
        // what's actually intended above
        assert(m_distinctNode);

        // Retrieve the distinct column index from the target table
        // using the distinct column name stored in the inline
        // distinct node.
        //
        // Should this be done elsewhere?
        voltdb::Table *targetTable = m_node->getTargetTable();
        assert (targetTable);
        std::string distinctColumnName = m_distinctNode->getDistinctColumnName();        assert(!distinctColumnName.empty());

        int distinctColumn = targetTable->columnIndex(distinctColumnName);
        assert(distinctColumn != -1);
        m_distinctNode->setDistinctColumn(distinctColumn);

        // Use the information we just cached in the inline plan node.
        assert(m_distinctNode->getDistinctColumn() >= 0);
        m_distinctColumn = m_distinctNode->getDistinctColumn();
        m_distinctColumnType =
            m_node->getTargetTable()->schema()->columnType(m_distinctColumn);

        //
        // HACK: For now use integer types only
        //
        if ((m_distinctColumnType != VALUE_TYPE_TINYINT) &&
            (m_distinctColumnType != VALUE_TYPE_SMALLINT) &&
            (m_distinctColumnType != VALUE_TYPE_INTEGER) &&
            (m_distinctColumnType != VALUE_TYPE_BIGINT)) {
            VOLT_ERROR("The Distinct operation is not supported for '%s'"
                       " columns", getTypeName(m_distinctColumnType).c_str());
            delete [] m_projectionExpressions;
            return false;
        }
    }

    //
    // INLINE LIMIT
    //
    if (m_node->getInlinePlanNode(PLAN_NODE_TYPE_LIMIT) != NULL)
    {
        m_limitNode =
            static_cast<LimitPlanNode*>
            (m_node->getInlinePlanNode(PLAN_NODE_TYPE_LIMIT));
    }

    //
    // Make sure that we have search keys and that they're not null
    //
    m_numOfSearchkeys = (int)m_node->getSearchKeyExpressions().size();
    m_searchKeyBeforeSubstituteArrayPtr =
      boost::shared_array<AbstractExpression*>
        (new AbstractExpression*[m_numOfSearchkeys]);
    m_searchKeyBeforeSubstituteArray = m_searchKeyBeforeSubstituteArrayPtr.get();
    m_searchKeyAllParamArrayPtr =
        expressionutil::
        convertIfAllParameterValues(m_node->getSearchKeyExpressions());
    m_searchKeyAllParamArray = m_searchKeyAllParamArrayPtr.get();
    m_needsSubstituteSearchKeyPtr =
        boost::shared_array<bool>(new bool[m_numOfSearchkeys]);
    m_needsSubstituteSearchKey = m_needsSubstituteSearchKeyPtr.get();
    // if (m_numOfSearchkeys == 0)
    // {
    //     VOLT_ERROR("There are no search key expressions for PlanNode '%s'",
    //                m_node->debug().c_str());
    //     return false;
    // }
    for (int ctr = 0; ctr < m_numOfSearchkeys; ctr++)
    {
        if (m_node->getSearchKeyExpressions()[ctr] == NULL)
        {
            VOLT_ERROR("The search key expression at position '%d' is NULL for"
                       " PlanNode '%s'", ctr, m_node->debug().c_str());
            delete [] m_projectionExpressions;
            return false;
        }
        m_needsSubstituteSearchKeyPtr[ctr] =
            m_node->getSearchKeyExpressions()[ctr]->hasParameter();
        m_searchKeyBeforeSubstituteArrayPtr[ctr] =
            m_node->getSearchKeyExpressions()[ctr];
    }

    //
    // Initialize local variables
    //

    //output table should be temptable
    m_outputTable = static_cast<TempTable*>(m_node->getOutputTable());
    //target table should be persistenttable
    m_targetTable = static_cast<PersistentTable*>(m_node->getTargetTable());
    m_numOfColumns = static_cast<int>(m_outputTable->columnCount());
    
    // the catalog table is used to get the relativeIndex of the targetTable
    m_catalogTable = catalogDb->tables().get(m_targetTable->name());
    

    //
    // Grab the Index from our inner table
    // We'll throw an error if the index is missing
    //
    m_index = m_targetTable->index(m_node->getTargetIndexName());
    if (m_index == NULL) {
        VOLT_ERROR("Failed to retreive index '%s' from table '%s' for PlanNode"
                   " '%s'", m_node->getTargetIndexName().c_str(),
                   m_targetTable->name().c_str(), m_node->debug().c_str());
        // delete [] m_searchKeyBackingStore;
        delete [] m_projectionExpressions;
        return false;
    }
    m_searchKey = TableTuple(m_index->getKeySchema());
    m_searchKeyBackingStore = new char[m_index->getKeySchema()->tupleLength()];
    m_searchKey.moveNoHeader(m_searchKeyBackingStore);
    
    m_tuple = TableTuple(m_targetTable->schema());

    if (m_node->getEndExpression() != NULL)
    {
        m_needsSubstituteEndExpression =
            m_node->getEndExpression()->hasParameter();
    }
    if (m_node->getPredicate() != NULL)
    {
        m_needsSubstitutePostExpression =
            m_node->getPredicate()->hasParameter();
    }

    //
    // INLINE AGGREGATE
    //
    m_aggregateColumnIdx = -1;
    m_aggregateCompareValue = VALUE_COMPARE_EQUAL;
    m_aggregateColumnType = VALUE_TYPE_INVALID;
    if (m_node->getInlinePlanNode(PLAN_NODE_TYPE_AGGREGATE) != NULL)
    {
        m_aggregateNode =
            static_cast<AggregatePlanNode*>
            (m_node->getInlinePlanNode(PLAN_NODE_TYPE_AGGREGATE));
        ExpressionType aggregateType = m_aggregateNode->getAggregates()[0];
        if ((aggregateType != EXPRESSION_TYPE_AGGREGATE_MIN) &&
            (aggregateType != EXPRESSION_TYPE_AGGREGATE_MAX))
        {
            VOLT_ERROR("Unsupported inline Aggregate type '%s' in PlanNode '%s'",
                       expressionutil::getTypeName(aggregateType).c_str(),
                       m_node->debug().c_str());
            delete [] m_searchKeyBackingStore;
            delete [] m_projectionExpressions;
            return false;
        }

        //
        // HACKISH
        // For now it's hard-coded so that it only
        // process single-column-aggregate
        //
        std::vector<int> agg_column_idxes(1);
        agg_column_idxes[0] =
            m_targetTable->
            columnIndex(m_aggregateNode->getAggregateColumnNames()[0]);
        m_aggregateNode->setAggregateColumns(agg_column_idxes);
        m_aggregateColumnIdx = m_aggregateNode->getAggregateColumns()[0];
        m_aggregateCompareValue =
            (aggregateType == EXPRESSION_TYPE_AGGREGATE_MIN ?
             VALUE_COMPARE_LESSTHAN
             : VALUE_COMPARE_GREATERTHAN);
        m_aggregateColumnType =
            m_targetTable->schema()->columnType(m_aggregateColumnIdx);
    }

    //
    // Miscellanous Information
    //
    m_lookupType = m_node->getLookupType();
    m_sortDirection = m_node->getSortDirection();

    return true;
}

bool IndexScanExecutor::p_execute(const NValueArray &params, ReadWriteTracker *tracker)
{
    assert(m_node);
    assert(m_node == dynamic_cast<IndexScanPlanNode*>(abstract_node));
    assert(m_outputTable);
    assert(m_outputTable == static_cast<TempTable*>(m_node->getOutputTable()));
    assert(m_targetTable);
    assert(m_targetTable == m_node->getTargetTable());
    VOLT_TRACE("IndexScan: %s.%s", m_targetTable->name().c_str(),
               m_index->getName().c_str());

    // INLINE PROJECTION
    // Set params to expression tree via substitute()
    assert(m_numOfColumns == m_outputTable->columnCount());
    if (m_projectionNode != NULL && m_projectionAllTupleArray == NULL)
    {
        for (int ctr = 0; ctr < m_numOfColumns; ctr++)
        {
            assert(m_projectionNode->getOutputColumnExpressions()[ctr]);
            if (m_needsSubstituteProject[ctr])
            {
                m_projectionExpressions[ctr]->substitute(params);
            }
            assert(m_projectionExpressions[ctr]);
        }
    }

    //
    // INLINE AGGREGATE
    // We can also perform a really simple inline aggregate to get a
    // single min or max value of the input table.
    //
    bool aggregate_isset = false;
    NValue aggregate_value;
    void* aggregate_tuple_address = NULL;

    //
    // INLINE DISTINCT
    //
    if (m_distinctNode != NULL)
    {
        m_distinctValueSet.clear();
    }

    //
    // INLINE LIMIT
    //
    if (m_limitNode != NULL)
    {
        m_limitNode->getLimitAndOffsetByReference(params, m_limitSize, m_limitOffset);
        if (m_limitOffset > 0)
        {
            VOLT_ERROR("The limit offset operation is not supported for"
                       " IndexScans yet");
            return false;
        }
    }

    //
    // SEARCH KEY
    //
    // assert (m_searchKey.getSchema()->columnCount() == m_numOfSearchkeys ||
    //         m_lookupType == INDEX_LOOKUP_TYPE_GT);
    m_searchKey.setAllNulls();
    if (m_searchKeyAllParamArray != NULL)
    {
        VOLT_TRACE("sweet, all params");
        for (int ctr = 0; ctr < m_numOfSearchkeys; ctr++)
        {
            m_searchKey.setNValue( ctr, params[m_searchKeyAllParamArray[ctr]]);
        }
    }
    else
    {
        for (int ctr = 0; ctr < m_numOfSearchkeys; ctr++) {
            if (m_needsSubstituteSearchKey[ctr]) {
                m_searchKeyBeforeSubstituteArray[ctr]->substitute(params);
            }
            m_searchKey.
              setNValue(ctr,
                        m_searchKeyBeforeSubstituteArray[ctr]->eval(&m_dummy, NULL));
        }
    }
    assert(m_searchKey.getSchema()->columnCount() > 0);

    //
    // END EXPRESSION
    //
    AbstractExpression* end_expression = m_node->getEndExpression();
    if (end_expression != NULL)
    {
        if (m_needsSubstituteEndExpression) {
            end_expression->substitute(params);
        }
        VOLT_TRACE("End Expression:\n%s", end_expression->debug(true).c_str());
    }

    //
    // POST EXPRESSION
    //
    AbstractExpression* post_expression = m_node->getPredicate();
    if (post_expression != NULL)
    {
        if (m_needsSubstitutePostExpression) {
            post_expression->substitute(params);
        }
        VOLT_DEBUG("Post Expression:\n%s", post_expression->debug(true).c_str());
    }

    assert (m_index);
    assert (m_index == m_targetTable->index(m_node->getTargetIndexName()));

    int tuples_written = 0;

    //
    // An index scan has three parts:
    //  (1) Lookup tuples using the search key
    //  (2) For each tuple that comes back, check whether the
    //  end_expression is false.
    //  If it is, then we stop scanning. Otherwise...
    //  (3) Check whether the tuple satisfies the post expression.
    //      If it does, then add it to the output table
    //
    // Use our search key to prime the index iterator
    // Now loop through each tuple given to us by the iterator
    //
    if (m_numOfSearchkeys > 0)
    {
        if (m_lookupType == INDEX_LOOKUP_TYPE_EQ)
        {
            m_index->moveToKey(&m_searchKey);
        }
        else if (m_lookupType == INDEX_LOOKUP_TYPE_GT)
        {
            m_index->moveToGreaterThanKey(&m_searchKey);
        }
        else if (m_lookupType == INDEX_LOOKUP_TYPE_GTE)
        {
            m_index->moveToKeyOrGreater(&m_searchKey);
        }
        else
        {
            VOLT_ERROR("Unexpected IndexLookupType: %s", indexLookupToString(m_lookupType).c_str());
            return false;
        }
    }
    VOLT_TRACE("IndexLookupType: %s SortDirectionType: %s", indexLookupToString(m_lookupType).c_str(),
                                                            sortDirectionToString(m_sortDirection).c_str());

    if (m_sortDirection != SORT_DIRECTION_TYPE_INVALID) {
        bool order_by_asc = true;

        if (m_sortDirection == SORT_DIRECTION_TYPE_ASC) {
            // nothing now
        } else {
            order_by_asc = false;
        }

        if (m_numOfSearchkeys == 0)
            m_index->moveToEnd(order_by_asc);
    } else if (m_sortDirection == SORT_DIRECTION_TYPE_INVALID &&
               m_numOfSearchkeys == 0) {
        VOLT_ERROR("Unexpected SortDirectionType: %s", sortDirectionToString(m_sortDirection).c_str());
        return false;
    }
    
    // Anti-Cache Variables
    #ifdef ANTICACHE
    AntiCacheEvictionManager* eviction_manager = m_targetTable->m_executorContext->getAntiCacheEvictionManager();
    bool hasEvictedTable = (eviction_manager != NULL && m_targetTable->getEvictedTable() != NULL);
    bool blockingMergeSuccessful = false;
    #endif

    //
    // We have to different nextValue() methods for different lookup types
    //
    while ((m_lookupType == INDEX_LOOKUP_TYPE_EQ &&
            !(m_tuple = m_index->nextValueAtKey()).isNullTuple()) ||
           ((m_lookupType != INDEX_LOOKUP_TYPE_EQ || m_numOfSearchkeys == 0) &&
            !(m_tuple = m_index->nextValue()).isNullTuple()))
    {
        blockingMergeSuccessful = false;
        m_targetTable->updateTupleAccessCount();
        
        // Read/Write Set Tracking
        if (tracker != NULL) {
            tracker->markTupleRead(m_targetTable, &m_tuple);
        }
        
        #ifdef ANTICACHE
        // We are pointing to an entry for an evicted tuple
        if (hasEvictedTable && m_tuple.isEvicted()) {
            VOLT_DEBUG("Tuple in index scan on %s is evicted. Current txn will have to be restarted...",
                       m_targetTable->name().c_str());      

            // Tell the EvictionManager's internal tracker that we touched this mofo
            VOLT_DEBUG("%s",m_tuple.getSchema()->debug().c_str());
            eviction_manager->recordEvictedAccess(m_catalogTable, &m_tuple);
            VOLT_DEBUG("recorded the evicted access in table %s\n", m_catalogTable->name().c_str());
            // Pavlo: 2014-07-09
            // If the tuple is evicted, then we can't continue with the rest of stuff below us.
            // There is nothing else we can do with it (i.e., check expressions).
            // I don't know why this wasn't here in the first place?

            // MJG: 2015-02-20
            // If we can merge now, let's merge
            // TODO: possibly an alternate codepath that simply looks through all the tuples
            // for evicted tuples and then see if we have any non-blockable accesses
            if (eviction_manager->hasBlockableEvictedAccesses()) {
                blockingMergeSuccessful = eviction_manager->blockingMerge();
            } else {
                blockingMergeSuccessful = false;
                continue;
            }
        }
        
        // MJG TEST: If we merged, maybe we need to grab the tuple again. Let's try
        // we need to check again which way we want to get the index based upon the 
        // INDEX_LOOKUP_TYPE. For now it works with != EQ
        if (blockingMergeSuccessful) {
            VOLT_TRACE("grabbing tuple again");
            if (m_lookupType == INDEX_LOOKUP_TYPE_EQ) {
                m_index->moveToKey(&m_searchKey);
                m_tuple = m_index->nextValueAtKey();
            } else {
                if (m_lookupType == INDEX_LOOKUP_TYPE_GT) 
                    m_index->moveToGreaterThanKey(&m_searchKey);
                else 
                    m_index->moveToKeyOrGreater(&m_searchKey);
                m_tuple = m_index->nextValue();
            }
                
            if (m_tuple.isNullTuple()) {
                VOLT_INFO("We've got a null tuple for some reason");
            }
        }

        VOLT_TRACE("Merged Tuple: %s", m_tuple.debug(m_targetTable->name()).c_str());
        #endif        
        //
        // First check whether the end_expression is now false
        //
        if (end_expression != NULL &&
            end_expression->eval(&m_tuple, NULL).isFalse()) {
            VOLT_DEBUG("End Expression evaluated to false, stopping scan");
            break;
        }
        
        if (blockingMergeSuccessful) {
            VOLT_DEBUG("tuple merged and End Expression evaluated to true, continuing scan");
        }
        //
        // Then apply our post-predicate to do further filtering
        //
        if (post_expression == NULL ||
            post_expression->eval(&m_tuple, NULL).isTrue()) {

            #ifdef ANTICACHE
            if (hasEvictedTable) {
                // update the tuple in the LRU eviction chain
                eviction_manager->updateTuple(m_targetTable, &m_tuple, false);
            }
            #endif
            
            //
            // Inline Distinct
            //
            if (m_distinctNode != NULL) {
                NValue value = m_tuple.getNValue(m_distinctColumn);
                // insert returns a pair<iterator, bool_succeeded>.
                // Don't want to continue if insert failed (value
                // was already present).
                if (m_distinctValueSet.insert(value).second == false) {
                    continue;
                }
            }
            //
            // Inline Aggregate
            //
            if (m_aggregateNode != NULL) {
                // search for a min or max value.
                // m_aggregateCompareValue is either "greater-than" or
                // "less-than".
                if (aggregate_isset == false ||
                    m_aggregateCompareValue == VALUE_COMPARE_LESSTHAN ?
                    m_tuple.getNValue(m_aggregateColumnIdx).op_lessThan(aggregate_value).isTrue() :
                    m_tuple.getNValue(m_aggregateColumnIdx).op_greaterThan(aggregate_value).isTrue())
                {
                    aggregate_value = m_tuple.getNValue(m_aggregateColumnIdx);
                    aggregate_tuple_address = m_tuple.address();
                    aggregate_isset = true;
                }
            //
            // Inline Projection
            // Project (or replace) values from input tuple
            //
            } else if (m_projectionNode != NULL) {
                TableTuple &temp_tuple = m_outputTable->tempTuple();
                if (m_projectionAllTupleArray != NULL) {
                    VOLT_DEBUG("sweet, all tuples");
                    for (int ctr = m_numOfColumns - 1; ctr >= 0; --ctr) {
                        temp_tuple.setNValue(ctr,
                                             m_tuple.getNValue(m_projectionAllTupleArray[ctr]));
                    }
                } else {
                    for (int ctr = m_numOfColumns - 1; ctr >= 0; --ctr) {
                        temp_tuple.setNValue(ctr,
                                             m_projectionExpressions[ctr]->eval(&m_tuple, NULL));
                    }
                }
                m_outputTable->insertTupleNonVirtual(temp_tuple);
                tuples_written++;
            //
            // Straight Insert
            //
            } else {
                //
                // Try to put the tuple into our output table
                //
                m_outputTable->insertTupleNonVirtual(m_tuple);
                tuples_written++;
            }
            
            //
            // INLINE LIMIT
            //
            if (m_limitNode != NULL && tuples_written >= m_limitSize) {
                VOLT_DEBUG("Hit limit of %d tuples. Halting scan", tuples_written);
                break;
            }
        }
    } // WHILE

    //
    // Inline Aggregate
    //
    if (m_aggregateNode != NULL && aggregate_isset) {
        m_tuple.move(aggregate_tuple_address);
        //
        // Inline Projection
        //
        if (m_projectionNode != NULL) {
            TableTuple &temp_tuple = m_outputTable->tempTuple();
            if (m_projectionAllTupleArray != NULL) {
                for (int ctr = m_numOfColumns - 1; ctr >= 0; --ctr) {
                    temp_tuple.setNValue(ctr,
                                         m_tuple.getNValue(m_projectionAllTupleArray[ctr]));
                }
            } else {
                for (int ctr = m_numOfColumns - 1; ctr >= 0; --ctr) {
                    temp_tuple.setNValue(ctr,
                                         m_projectionExpressions[ctr]->eval(&m_tuple, NULL));
                }
            }
            m_outputTable->insertTupleNonVirtual(temp_tuple);
            #ifdef ANTICACHE
            if (hasEvictedTable) {
                // update the tuple in the LRU eviction chain
                eviction_manager->updateTuple(m_targetTable, &m_tuple, false);
            }
            #endif
        //
        // Straight Insert
        //
        } else {
            m_outputTable->insertTupleNonVirtual(m_tuple);
            #ifdef ANTICACHE
            if (hasEvictedTable) {
                // update the tuple in the LRU eviction chain
                eviction_manager->updateTuple(m_targetTable, &m_tuple, false);
            }
            #endif
        }
    }
    
    #ifdef ANTICACHE
    // throw exception indicating evicted blocks are needed
    if (hasEvictedTable && !blockingMergeSuccessful && eviction_manager->hasEvictedAccesses()) {
        VOLT_DEBUG("Throwing EvictedaccessException\n");
        eviction_manager->throwEvictedAccessException();
    }
    #endif
    
    VOLT_TRACE("Index Scanned :\n %s", m_outputTable->debug().c_str());
    return true;
}

IndexScanExecutor::~IndexScanExecutor() {
    delete [] m_searchKeyBackingStore;
    delete [] m_projectionExpressions;
}

/**
 * Copyright (c) 2023 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifdef MINER_SCHEMA_DEF

MINER_SCHEMA_DEF(TENANT_ID, 0)
MINER_SCHEMA_DEF(TRANS_ID, 1)
MINER_SCHEMA_DEF(PRIMARY_KEY, 2)
// MINER_SCHEMA_DEF(ROW_UNIQUE_ID, 3)
// MINER_SCHEMA_DEF(SEQ_NO, 4)
MINER_SCHEMA_DEF(TENANT_NAME, 5)
MINER_SCHEMA_DEF(DATABASE_NAME, 6)
MINER_SCHEMA_DEF(TABLE_NAME, 7)
MINER_SCHEMA_DEF(OPERATION, 8)
MINER_SCHEMA_DEF(OPERATION_CODE, 9)
MINER_SCHEMA_DEF(COMMIT_SCN, 10)
MINER_SCHEMA_DEF(COMMIT_TIMESTAMP, 11)
MINER_SCHEMA_DEF(SQL_REDO, 12)
MINER_SCHEMA_DEF(SQL_UNDO, 13)
MINER_SCHEMA_DEF(ORG_CLUSTER_ID, 14)

#endif
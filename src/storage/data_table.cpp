#include "storage/data_table.hpp"

#include "catalog/catalog_entry/table_catalog_entry.hpp"
#include "common/exception.hpp"
#include "common/helper.hpp"
#include "common/vector_operations/vector_operations.hpp"
#include "common/types/static_vector.hpp"
#include "execution/expression_executor.hpp"
#include "main/client_context.hpp"
#include "planner/constraints/list.hpp"
#include "transaction/transaction.hpp"
#include "transaction/transaction_manager.hpp"

#include "transaction/version_info.hpp"

using namespace duckdb;
using namespace std;

DataTable::DataTable(StorageManager &storage, string schema, string table, vector<TypeId> types_)
    : cardinality(0), schema(schema), table(table), types(types_), serializer(types), storage(storage) {
	index_t accumulative_size = 0;
	for (index_t i = 0; i < types.size(); i++) {
		accumulative_tuple_size.push_back(accumulative_size);
		accumulative_size += GetTypeIdSize(types[i]);
	}
	tuple_size = accumulative_size;
	// create empty statistics for the table
	statistics = unique_ptr<ColumnStatistics[]>(new ColumnStatistics[types.size()]);
	// and an empty column chunk for each column
	columns = unique_ptr<SegmentTree[]>(new SegmentTree[types.size()]);
	for (index_t i = 0; i < types.size(); i++) {
		columns[i].AppendSegment(make_unique<ColumnSegment>(0));
	}
	// initialize the table with an empty storage chunk
	AppendVersionChunk(0);
}

VersionChunk *DataTable::AppendVersionChunk(index_t start) {
	auto chunk = make_unique<VersionChunk>(*this, start);
	auto chunk_pointer = chunk.get();
	// set the columns of the chunk
	chunk->columns = unique_ptr<ColumnPointer[]>(new ColumnPointer[types.size()]);
	for (index_t i = 0; i < types.size(); i++) {
		chunk->columns[i].segment = (ColumnSegment *)columns[i].nodes.back().node;
		chunk->columns[i].offset = chunk->columns[i].segment->count;
	}
	storage_tree.AppendSegment(move(chunk));
	return chunk_pointer;
}

VersionChunk *DataTable::GetChunk(index_t row_number) {
	return (VersionChunk *)storage_tree.GetSegment(row_number);
}

void DataTable::AppendVector(index_t column_index, Vector &data, index_t offset, index_t count) {
	// get the segment to append to
	auto segment = (ColumnSegment *)columns[column_index].GetLastSegment();
	// append the data from the vector
	// first check how much we can append to the column segment
	index_t type_size = GetTypeIdSize(types[column_index]);
	index_t start_position = segment->offset;
	data_ptr_t target = segment->GetData() + start_position;
	index_t elements_to_copy = std::min((BLOCK_SIZE - start_position) / type_size, count);
	if (elements_to_copy > 0) {
		// we can fit elements in the current column segment: copy them there
		VectorOperations::CopyToStorage(data, target, offset, elements_to_copy);
		offset += elements_to_copy;
		segment->count += elements_to_copy;
		segment->offset += elements_to_copy * type_size;
	}
	if (elements_to_copy < count) {
		// we couldn't fit everything we wanted in the original column segment
		// create a new one
		auto column_segment = make_unique<ColumnSegment>(segment->start + segment->count);
		columns[column_index].AppendSegment(move(column_segment));
		// now try again
		AppendVector(column_index, data, offset, count - elements_to_copy);
	}
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
static void VerifyNotNullConstraint(TableCatalogEntry &table, Vector &vector, string &col_name) {
	if (VectorOperations::HasNull(vector)) {
		throw ConstraintException("NOT NULL constraint failed: %s.%s", table.name.c_str(), col_name.c_str());
	}
}

static void VerifyCheckConstraint(TableCatalogEntry &table, Expression &expr, DataChunk &chunk) {
	ExpressionExecutor executor(chunk);
	Vector result(TypeId::INTEGER, true, false);
	try {
		executor.ExecuteExpression(expr, result);
	} catch (Exception &ex) {
		throw ConstraintException("CHECK constraint failed: %s (Error: %s)", table.name.c_str(), ex.what());
	} catch (...) {
		throw ConstraintException("CHECK constraint failed: %s (Unknown Error)", table.name.c_str());
	}

	int *dataptr = (int *)result.data;
	for (index_t i = 0; i < result.count; i++) {
		index_t index = result.sel_vector ? result.sel_vector[i] : i;
		if (!result.nullmask[index] && dataptr[index] == 0) {
			throw ConstraintException("CHECK constraint failed: %s", table.name.c_str());
		}
	}
}

static void VerifyUniqueConstraint(TableCatalogEntry &table, unordered_set<index_t> &keys, DataChunk &chunk) {
	// not implemented for multiple keys
	assert(keys.size() == 1);
	// check if the columns are unique
	for (auto &key : keys) {
		if (!VectorOperations::Unique(chunk.data[key])) {
			throw ConstraintException("duplicate key value violates primary key or unique constraint");
		}
	}
}

void DataTable::VerifyAppendConstraints(TableCatalogEntry &table, DataChunk &chunk) {
	for (auto &constraint : table.bound_constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null = *reinterpret_cast<BoundNotNullConstraint *>(constraint.get());
			VerifyNotNullConstraint(table, chunk.data[not_null.index], table.columns[not_null.index].name);
			break;
		}
		case ConstraintType::CHECK: {
			auto &check = *reinterpret_cast<BoundCheckConstraint *>(constraint.get());
			VerifyCheckConstraint(table, *check.expression, chunk);
			break;
		}
		case ConstraintType::UNIQUE: {
			// we check these constraint in the unique index
			auto &unique = *reinterpret_cast<BoundUniqueConstraint *>(constraint.get());
			VerifyUniqueConstraint(table, unique.keys, chunk);
			break;
		}
		case ConstraintType::FOREIGN_KEY:
		default:
			throw NotImplementedException("Constraint type not implemented!");
		}
	}
}

void DataTable::AppendToIndexes(DataChunk &chunk, row_t row_start) {
	if (indexes.size() == 0) {
		return;
	}
	// first generate the vector of row identifiers
	StaticVector<row_t> row_identifiers;
	row_identifiers.sel_vector = chunk.sel_vector;
	row_identifiers.count = chunk.size();
	VectorOperations::GenerateSequence(row_identifiers, row_start);

	index_t failed_index = INVALID_INDEX;
	// now append the entries to the indices
	for (index_t i = 0; i < indexes.size(); i++) {
		if (!indexes[i]->Append(chunk, row_identifiers)) {
			failed_index = i;
			break;
		}
	}
	if (failed_index != INVALID_INDEX) {
		// constraint violation!
		// remove any appended entries from previous indexes (if any)
		for (index_t i = 0; i < failed_index; i++) {
			indexes[i]->Delete(chunk, row_identifiers);
		}
		throw ConstraintException("PRIMARY KEY or UNIQUE constraint violated: duplicated key");
	}
}

void DataTable::Append(TableCatalogEntry &table, ClientContext &context, DataChunk &chunk) {
	if (chunk.size() == 0) {
		return;
	}
	if (chunk.column_count != table.columns.size()) {
		throw CatalogException("Mismatch in column count for append");
	}

	chunk.Verify();

	// verify any constraints on the new chunk
	VerifyAppendConstraints(table, chunk);

	StringHeap heap;
	chunk.MoveStringsToHeap(heap);

	row_t row_start;
	{
		// ready to append: obtain an exclusive lock on the last segment
		lock_guard<mutex> tree_lock(storage_tree.node_lock);
		auto last_chunk = (VersionChunk *)storage_tree.nodes.back().node;
		auto lock = last_chunk->lock.GetExclusiveLock();
		assert(!last_chunk->next);

		// get the start row_id of the chunk
		row_start = last_chunk->start + last_chunk->count;

		// Append the entries to the indexes, we do this first because this might fail in case of unique index conflicts
		AppendToIndexes(chunk, row_start);

		// update the statistics with the new data
		for (index_t i = 0; i < types.size(); i++) {
			statistics[i].Update(chunk.data[i]);
		}

		Transaction &transaction = context.ActiveTransaction();
		index_t remainder = chunk.size();
		index_t offset = 0;
		while (remainder > 0) {
			index_t to_copy = min(STORAGE_CHUNK_SIZE - last_chunk->count, remainder);
			if (to_copy > 0) {
				// push deleted entries into the undo buffer
				last_chunk->PushDeletedEntries(transaction, to_copy);
				// now insert the elements into the column segments
				for (index_t i = 0; i < chunk.column_count; i++) {
					AppendVector(i, chunk.data[i], offset, to_copy);
				}
				// now increase the count of the chunk
				last_chunk->count += to_copy;
				offset += to_copy;
				remainder -= to_copy;
			}
			if (remainder > 0) {
				last_chunk = AppendVersionChunk(last_chunk->start + last_chunk->count);
			}
		}

		// after an append move the strings to the chunk
		last_chunk->string_heap.MergeHeap(heap);
	}
}

//===--------------------------------------------------------------------===//
// Delete
//===--------------------------------------------------------------------===//
void DataTable::Delete(TableCatalogEntry &table, ClientContext &context, Vector &row_identifiers) {
	assert(row_identifiers.type == ROW_TYPE);
	if (row_identifiers.count == 0) {
		return;
	}

	Transaction &transaction = context.ActiveTransaction();

	auto ids = (row_t *)row_identifiers.data;
	auto sel_vector = row_identifiers.sel_vector;
	auto first_id = sel_vector ? ids[sel_vector[0]] : ids[0];
	// first find the chunk the row ids belong to
	auto chunk = GetChunk(first_id);

	// get an exclusive lock on the chunk
	auto lock = chunk->lock.GetExclusiveLock();
	// no constraints are violated
	// now delete the entries
	VectorOperations::Exec(row_identifiers, [&](index_t i, index_t k) {
		auto id = ids[i] - chunk->start;
		// assert that all ids in the vector belong to the same storage
		// chunk
		assert(id < chunk->count);
		// check for conflicts
		auto version = chunk->GetVersionInfo(id);
		if (version) {
			if (version->version_number >= TRANSACTION_ID_START &&
			    version->version_number != transaction.transaction_id) {
				throw TransactionException("Conflict on tuple deletion!");
			}
		}
		// no conflict, move the current tuple data into the undo buffer
		chunk->PushTuple(transaction, UndoFlags::DELETE_TUPLE, id);
		// and set the deleted flag
		chunk->SetDeleted(id);
	});
}

//===--------------------------------------------------------------------===//
// Update
//===--------------------------------------------------------------------===//
static void CreateMockChunk(TableCatalogEntry &table, vector<column_t> &column_ids, DataChunk &chunk,
                            DataChunk &mock_chunk) {
	// construct a mock DataChunk
	auto types = table.GetTypes();
	mock_chunk.InitializeEmpty(types);
	for (column_t i = 0; i < column_ids.size(); i++) {
		mock_chunk.data[column_ids[i]].Reference(chunk.data[i]);
		mock_chunk.sel_vector = mock_chunk.data[column_ids[i]].sel_vector;
	}
	mock_chunk.data[0].count = chunk.size();
}

static bool CreateMockChunk(TableCatalogEntry &table, vector<column_t> &column_ids,
                            unordered_set<column_t> &desired_column_ids, DataChunk &chunk, DataChunk &mock_chunk) {
	index_t found_columns = 0;
	// check whether the desired columns are present in the UPDATE clause
	for (column_t i = 0; i < column_ids.size(); i++) {
		if (desired_column_ids.find(column_ids[i]) != desired_column_ids.end()) {
			found_columns++;
		}
	}
	if (found_columns == 0) {
		// no columns were found: no need to check the constraint again
		return false;
	}
	if (found_columns != desired_column_ids.size()) {
		// FIXME: not all columns in UPDATE clause are present!
		// this should not be triggered at all as the binder should add these columns
		throw NotImplementedException(
		    "Not all columns required for the CHECK constraint are present in the UPDATED chunk!");
	}
	// construct a mock DataChunk
	CreateMockChunk(table, column_ids, chunk, mock_chunk);
	return true;
}

void DataTable::VerifyUpdateConstraints(TableCatalogEntry &table, DataChunk &chunk, vector<column_t> &column_ids) {
	for (auto &constraint : table.bound_constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL: {
			auto &not_null = *reinterpret_cast<BoundNotNullConstraint *>(constraint.get());
			// check if the constraint is in the list of column_ids
			for (index_t i = 0; i < column_ids.size(); i++) {
				if (column_ids[i] == not_null.index) {
					// found the column id: check the data in
					VerifyNotNullConstraint(table, chunk.data[i], table.columns[not_null.index].name);
					break;
				}
			}
			break;
		}
		case ConstraintType::CHECK: {
			auto &check = *reinterpret_cast<BoundCheckConstraint *>(constraint.get());

			DataChunk mock_chunk;
			if (CreateMockChunk(table, column_ids, check.bound_columns, chunk, mock_chunk)) {
				VerifyCheckConstraint(table, *check.expression, mock_chunk);
			}
			break;
		}
		case ConstraintType::UNIQUE: {
			// we check these constraint in the unique index
			auto &unique = *reinterpret_cast<BoundUniqueConstraint *>(constraint.get());
			DataChunk mock_chunk;
			if (CreateMockChunk(table, column_ids, unique.keys, chunk, mock_chunk)) {
				VerifyUniqueConstraint(table, unique.keys, mock_chunk);
			}
			break;
		}
		case ConstraintType::FOREIGN_KEY:
			break;
		default:
			throw NotImplementedException("Constraint type not implemented!");
		}
	}
}

void DataTable::UpdateIndexes(TableCatalogEntry &table, vector<column_t> &column_ids, DataChunk &updates,
                              Vector &row_identifiers) {
	if (indexes.size() == 0) {
		return;
	}
	// first create a mock chunk to be used in the index appends
	DataChunk mock_chunk;
	CreateMockChunk(table, column_ids, updates, mock_chunk);

	index_t failed_index = INVALID_INDEX;
	// now insert the updated values into the index
	for (index_t i = 0; i < indexes.size(); i++) {
		// first check if the index is affected by the update
		if (!indexes[i]->IndexIsUpdated(column_ids)) {
			continue;
		}
		// if it is, we append the data to the index
		if (!indexes[i]->Append(mock_chunk, row_identifiers)) {
			failed_index = i;
			break;
		}
	}
	if (failed_index != INVALID_INDEX) {
		// constraint violation!
		// remove any appended entries from previous indexes (if any)
		for (index_t i = 0; i < failed_index; i++) {
			if (indexes[i]->IndexIsUpdated(column_ids)) {
				indexes[i]->Delete(mock_chunk, row_identifiers);
			}
		}
		throw ConstraintException("PRIMARY KEY or UNIQUE constraint violated: duplicated key");
	}
}

void DataTable::Update(TableCatalogEntry &table, ClientContext &context, Vector &row_identifiers,
                       vector<column_t> &column_ids, DataChunk &updates) {
	assert(row_identifiers.type == ROW_TYPE);
	updates.Verify();
	if (row_identifiers.count == 0) {
		return;
	}

	// first verify that no constraints are violated
	VerifyUpdateConstraints(table, updates, column_ids);

	// move strings to a temporary heap
	StringHeap heap;
	updates.MoveStringsToHeap(heap);

	// now perform the actual update
	Transaction &transaction = context.ActiveTransaction();

	auto ids = (row_t *)row_identifiers.data;
	auto sel_vector = row_identifiers.sel_vector;
	auto first_id = sel_vector ? ids[sel_vector[0]] : ids[0];
	// first find the chunk the row ids belong to
	auto chunk = GetChunk(first_id);

	// get an exclusive lock on the chunk
	auto lock = chunk->lock.GetExclusiveLock();

	// now update the entries
	// first check for any conflicts before we insert anything into the undo buffer
	// we check for any conflicts in ALL tuples first before inserting anything into the undo buffer
	// to make sure that we complete our entire update transaction after we do
	// this prevents inconsistencies after rollbacks, etc...
	VectorOperations::Exec(row_identifiers, [&](index_t i, index_t k) {
		auto id = ids[i] - chunk->start;
		// assert that all ids in the vector belong to the same chunk
		assert(id < chunk->count);
		// check for version conflicts
		auto version = chunk->GetVersionInfo(id);
		if (version) {
			if (version->version_number >= TRANSACTION_ID_START &&
			    version->version_number != transaction.transaction_id) {
				throw TransactionException("Conflict on tuple update!");
			}
		}
	});

	// now we update any indexes, we do this before inserting anything into the undo buffer
	UpdateIndexes(table, column_ids, updates, row_identifiers);

	// now we know there are no conflicts, move the tuples into the undo buffer and mark the chunk as dirty
	VectorOperations::Exec(row_identifiers, [&](index_t i, index_t k) {
		auto id = ids[i] - chunk->start;
		// move the current tuple data into the undo buffer
		chunk->PushTuple(transaction, UndoFlags::UPDATE_TUPLE, id);
	});

	// now update the columns in the base table
	for (index_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
		auto column_id = column_ids[col_idx];
		auto size = GetTypeIdSize(updates.data[col_idx].type);

		Vector *update_vector = &updates.data[col_idx];
		Vector null_vector;
		if (update_vector->nullmask.any()) {
			// has NULL values in the nullmask
			// copy them to a temporary vector
			null_vector.Initialize(update_vector->type, false);
			null_vector.count = update_vector->count;
			VectorOperations::CopyToStorage(*update_vector, null_vector.data);
			update_vector = &null_vector;
		}

		VectorOperations::Exec(row_identifiers, [&](index_t i, index_t k) {
			auto dataptr = chunk->GetPointerToRow(column_ids[col_idx], ids[i]);
			auto update_index = update_vector->sel_vector ? update_vector->sel_vector[k] : k;
			memcpy(dataptr, update_vector->data + update_index * size, size);
		});

		// update the statistics with the new data
		statistics[column_id].Update(updates.data[col_idx]);
	}
	// after a successful update move the strings into the chunk
	chunk->string_heap.MergeHeap(heap);
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void DataTable::InitializeScan(TableScanState &state) {
	state.chunk = (VersionChunk *)storage_tree.GetRootSegment();
	state.last_chunk = (VersionChunk *)storage_tree.GetLastSegment();
	state.last_chunk_count = state.last_chunk->count;
	state.columns = unique_ptr<ColumnPointer[]>(new ColumnPointer[types.size()]);
	for (index_t i = 0; i < types.size(); i++) {
		state.columns[i].segment = (ColumnSegment *)columns[i].GetRootSegment();
		state.columns[i].offset = 0;
	}
	state.offset = 0;
	state.version_chain = nullptr;
}

void DataTable::Scan(Transaction &transaction, DataChunk &result, const vector<column_t> &column_ids,
                     TableScanState &state) {
	// scan the base table
	while (state.chunk) {
		auto current_chunk = state.chunk;

		// scan the current chunk
		bool is_last_segment = current_chunk->Scan(state, transaction, result, column_ids, state.offset);

		if (is_last_segment) {
			// last segment of this chunk: move to next segment
			if (state.chunk == state.last_chunk) {
				state.chunk = nullptr;
				break;
			} else {
				state.offset = 0;
				state.chunk = (VersionChunk *)current_chunk->next.get();
			}
		} else {
			// move to next segment in this chunk
			state.offset++;
		}
		if (result.size() > 0) {
			return;
		}
	}
}

void DataTable::Fetch(Transaction &transaction, DataChunk &result, vector<column_t> &column_ids,
                      Vector &row_identifiers) {
	assert(row_identifiers.type == ROW_TYPE);
	auto row_ids = (row_t *)row_identifiers.data;
	// sort the row identifiers first
	// this is done so we can minimize the amount of chunks that we lock
	sel_t sort_vector[STANDARD_VECTOR_SIZE];
	VectorOperations::Sort(row_identifiers, sort_vector);

	for (index_t i = 0; i < row_identifiers.count; i++) {
		auto row_id = row_ids[sort_vector[i]];
		auto chunk = GetChunk(row_id);
		auto lock = chunk->lock.GetSharedLock();

		assert((index_t)row_id >= chunk->start && (index_t)row_id < chunk->start + chunk->count);
		auto index = row_id - chunk->start;

		chunk->RetrieveTupleData(transaction, result, column_ids, index);
	}
}

void DataTable::InitializeIndexScan(IndexTableScanState &state) {
	InitializeScan(state);
	state.version_index = 0;
	state.version_offset = 0;
}

void DataTable::CreateIndexScan(IndexTableScanState &state, vector<column_t> &column_ids, DataChunk &result) {
	while (state.chunk) {
		auto current_chunk = state.chunk;

		bool chunk_exhausted = current_chunk->CreateIndexScan(state, column_ids, result);

		if (chunk_exhausted) {
			// exceeded this chunk, move to next one
			state.chunk = (VersionChunk *)state.chunk->next.get();
			state.offset = 0;
			state.version_index = 0;
			state.version_offset = 0;
			state.version_chain = nullptr;
		}
		if (result.size() > 0) {
			return;
		}
	}
}

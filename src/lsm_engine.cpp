// LSM - FILE BASED

#include "lsm_engine.h"
#include <fstream>

using namespace std;

void lsm_engine::group_commit() {

  while (ready) {
    //std::cout << "Syncing ! " << endl;

    // sync
    fs_log.sync();

    std::this_thread::sleep_for(std::chrono::milliseconds(conf.gc_interval));
  }
}

void lsm_engine::merge_check() {
  if (++merge_looper % conf.merge_interval == 0) {
    merge(false);
    merge_looper = 0;
  }
}

void lsm_engine::merge(bool force) {
  //std::cout << "Merging ! " << merge_looper << endl;

  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    table_index *p_index = tab->indices->at(0);

    pbtree<unsigned long, record*>* pm_map = p_index->pm_map;

    size_t compact_threshold = conf.merge_ratio * p_index->off_map->size();
    bool compact = (pm_map->size() > compact_threshold);

    // Check if need to merge
    if (force || compact) {

      pbtree<unsigned long, record*>::const_iterator itr;
      record *pm_rec, *fs_rec;
      unsigned long key;
      off_t storage_offset;
      std::string val;

      // All tuples in table
      for (itr = pm_map->begin(); itr != pm_map->end(); itr++) {
        key = (*itr).first;
        pm_rec = (*itr).second;

        fs_rec = NULL;

        // Check if we need to merge
        if (p_index->off_map->exists(key) != 0) {
          storage_offset = p_index->off_map->at(key);
          val = tab->fs_data.at(storage_offset);
          fs_rec = deserialize_to_record(val, tab->sptr, false);

          int num_cols = pm_rec->sptr->num_columns;
          for (int field_itr = 0; field_itr < num_cols; field_itr++) {
            fs_rec->set_data(field_itr, pm_rec);
          }

          val = serialize(fs_rec, tab->sptr, false);
          //LOG_INFO("Merge :: update :: val :: %s ", val.c_str());

          tab->fs_data.update(storage_offset, val);

          delete fs_rec;

        } else {
          // Insert tuple
          val = serialize(pm_rec, tab->sptr, false);
          //LOG_INFO("Merge :: insert new :: val :: %s ", val.c_str());

          storage_offset = tab->fs_data.push_back(val);
          p_index->off_map->insert(key, storage_offset);
        }

        delete pm_rec;
      }

      // Clear mem table
      vector<table_index*> indices = tab->indices->get_data();
      for (table_index* index : indices)
        index->pm_map->clear();

    }
  }

  // Truncate log
  //if (force)
  //  fs_log.truncate();
}

lsm_engine::lsm_engine(const config& _conf, bool _read_only)
    : conf(_conf),
      db(conf.db) {

  etype = engine_type::LSM;
  read_only = _read_only;
  fs_log.configure(conf.fs_path + "log");
  merge_looper = 0;

  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    std::string table_file_name = conf.fs_path + std::string(tab->table_name);
    tab->fs_data.configure(table_file_name, tab->max_tuple_size, false);
  }

  // Logger start
  if (!read_only) {
    gc = std::thread(&lsm_engine::group_commit, this);
  }
}

lsm_engine::~lsm_engine() {

  if (read_only)
    return;

  // Logger end
  ready = false;
  gc.join();

  merge(true);

  fs_log.sync();
  fs_log.close();

  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    tab->fs_data.sync();
    tab->fs_data.close();
  }

}

std::string lsm_engine::select(const statement& st) {
  LOG_INFO("Select");
  std::string val;

  record *rec_ptr = st.rec_ptr;
  record *pm_rec = NULL, *fs_rec = NULL;
  table *tab = db->tables->at(st.table_id);
  table_index *table_index = tab->indices->at(st.table_index_id);
  std::string key_str = get_data(rec_ptr, table_index->sptr);

  unsigned long key = hash_fn(key_str);
  off_t storage_offset;

  // Check if key exists in mem
  if (table_index->pm_map->exists(key) != 0) {
    LOG_INFO("Using mem table ");
    pm_rec = table_index->pm_map->at(key);
    //printf("pm_rec :: %p \n", pm_rec);
  }

  // Check if key exists in fs
  if (table_index->off_map->exists(key) != 0) {
    LOG_INFO("Using ss table ");
    storage_offset = table_index->off_map->at(key);
    val = tab->fs_data.at(storage_offset);
    fs_rec = deserialize_to_record(val, tab->sptr, false);
    //printf("fs_rec :: %p \n", fs_rec);
  }

  if (pm_rec != NULL && fs_rec == NULL) {
    // From Memtable
    val = serialize(pm_rec, st.projection, false);
  } else if (pm_rec == NULL && fs_rec != NULL) {
    // From SSTable
    val = serialize(fs_rec, st.projection, false);

    delete fs_rec;
  } else if (pm_rec != NULL && fs_rec != NULL) {
    // Merge
    int num_cols = pm_rec->sptr->num_columns;
    for (int field_itr = 0; field_itr < num_cols; field_itr++) {
      if (pm_rec->sptr->columns[field_itr].enabled)
        fs_rec->set_data(field_itr, pm_rec);
    }

    val = serialize(fs_rec, st.projection, false);
    delete fs_rec;
  }

  LOG_INFO("val : %s", val.c_str());
  //cout << "val : " << val << endl;

  return val;
}

int lsm_engine::insert(const statement& st) {
  LOG_INFO("Insert");
  record* after_rec = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;

  std::string key_str = get_data(after_rec, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);

  // Check if key exists
  if (indices->at(0)->pm_map->exists(key)
      || indices->at(0)->off_map->exists(key)) {
    delete after_rec;
    return EXIT_SUCCESS;
  }

  // Add log entry
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << serialize(after_rec, after_rec->sptr, true) << "\n";
  entry_str = entry_stream.str();

  // Add log entry
  fs_log.push_back(entry_str);

  // Add entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(after_rec, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->pm_map->insert(key, after_rec);
  }

  return EXIT_SUCCESS;
}

int lsm_engine::remove(const statement& st) {
  LOG_INFO("Remove");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  off_t log_offset;
  std::string val;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);

  // Check if key does not exist
  if (indices->at(0)->pm_map->exists(key) == 0
      && indices->at(0)->off_map->exists(key) == 0) {
    cout << "not found in either index " << endl;
    delete rec_ptr;
    return EXIT_SUCCESS;
  }

  // Add log entry
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << serialize(rec_ptr, rec_ptr->sptr, true) << "\n";

  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);

  // Remove entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(rec_ptr, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->pm_map->erase(key);
    indices->at(index_itr)->off_map->erase(key);
  }

  if (indices->at(0)->pm_map->exists(key) != 0) {
    record* before_rec = indices->at(0)->pm_map->at(key);
    delete before_rec;
  }

  return EXIT_SUCCESS;
}

int lsm_engine::update(const statement& st) {
  LOG_INFO("Update");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = db->tables->at(st.table_id)->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);
  off_t log_offset;
  std::string val;
  record* before_rec;
  bool existing_rec = false;
  void *before_field, *after_field;

  entry_stream.str("");

  // Check if key does not exist
  if (indices->at(0)->pm_map->exists(key) == 0) {
    //LOG_INFO("Key not found in mem table %lu ", key);
    before_rec = rec_ptr;

  } else {
    existing_rec = true;
    before_rec = indices->at(0)->pm_map->at(key);

    entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
                 << " " << serialize(before_rec, before_rec->sptr, true) << "\n";

    // Update existing record
    for (int field_itr : st.field_ids) {
      if (rec_ptr->sptr->columns[field_itr].inlined == 0) {
        before_field = before_rec->get_pointer(field_itr);
        delete ((char*) before_field);
      }

      before_rec->set_data(field_itr, rec_ptr);
    }
  }

  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << serialize(before_rec, before_rec->sptr, true) << "\n";
  entry_str = entry_stream.str();

  // Add log entry
  fs_log.push_back(entry_str);

  // Add entry in indices
  if (existing_rec == false) {
    for (index_itr = 0; index_itr < num_indices; index_itr++) {
      key_str = get_data(before_rec, indices->at(index_itr)->sptr);
      key = hash_fn(key_str);

      indices->at(index_itr)->pm_map->insert(key, before_rec);
    }
  }

  return EXIT_SUCCESS;
}

void lsm_engine::txn_begin() {
}

void lsm_engine::txn_end(bool commit) {

  if (!read_only)
    merge_check();
}


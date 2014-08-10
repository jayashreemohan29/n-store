// WAL LOGGING

#include "wal_engine.h"
#include <fstream>

using namespace std;

wal_engine::wal_engine(const config& _conf, bool _read_only, unsigned int _tid)
    : conf(_conf),
      db(conf.db),
      tid(_tid) {
  etype = engine_type::WAL;
  read_only = _read_only;
  fs_log.configure(conf.fs_path + "log");

  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    std::string table_file_name = conf.fs_path + std::string(tab->table_name);
    tab->fs_data.configure(table_file_name, tab->max_tuple_size, false);
  }

  // Logger start
  if (!read_only) {
    gc = std::thread(&wal_engine::group_commit, this);
    ready = true;
  }
}

wal_engine::~wal_engine() {

  // Logger end
  if (!read_only) {
    ready = false;
    gc.join();

    if (!conf.recovery) {
      fs_log.sync();
      fs_log.close();
    }

    vector<table*> tables = db->tables->get_data();
    for (table* tab : tables) {
      tab->fs_data.sync();
      tab->fs_data.close();
    }

  }

}

std::string wal_engine::select(const statement& st) {
  LOG_INFO("Select");
  std::string val;

  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  table_index* table_index = tab->indices->at(st.table_index_id);
  std::string key_str = serialize(rec_ptr, table_index->sptr);

  unsigned long key = hash_fn(key_str);
  off_t storage_offset;

  if ((table_index->off_map->at(key, &storage_offset)) == false) {
    delete rec_ptr;
    return val;
  }

  val = tab->fs_data.at(storage_offset);
  val = deserialize_to_string(val, st.projection);
  LOG_INFO("val : %s", val.c_str());

  delete rec_ptr;
  return val;
}

int wal_engine::insert(const statement& st) {
  LOG_INFO("Insert");
  record* after_rec = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;

  std::string key_str = serialize(after_rec, indices->at(0)->sptr);
  LOG_INFO("key_str :: %s", key_str.c_str());
  unsigned long key = hash_fn(key_str);

  // Check if key present
  if (indices->at(0)->off_map->exists(key)) {
    delete after_rec;
    return EXIT_SUCCESS;
  }

  // Add log entry
  std::string after_tuple = serialize(after_rec, after_rec->sptr);
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << after_tuple << "\n";
  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);
  off_t storage_offset;

  storage_offset = tab->fs_data.push_back(after_tuple);

  // Add entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = serialize(after_rec, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->off_map->insert(key, storage_offset);
  }

  delete after_rec;
  return EXIT_SUCCESS;
}

int wal_engine::remove(const statement& st) {
  LOG_INFO("Remove");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  record* before_rec = NULL;

  std::string key_str = serialize(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);
  off_t storage_offset;
  std::string val;

  // Check if key does not exist
  if (indices->at(0)->off_map->at(key, &storage_offset) == false) {
    delete rec_ptr;
    return EXIT_SUCCESS;
  }

  val = tab->fs_data.at(storage_offset);
  before_rec = deserialize(val, tab->sptr);

  // Add log entry
  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " " << serialize(before_rec, before_rec->sptr) << "\n";

  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);

  // Remove entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = serialize(rec_ptr, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->off_map->erase(key);
  }

  before_rec->clear_data();
  delete before_rec;
  delete rec_ptr;
  return EXIT_SUCCESS;
}

int wal_engine::update(const statement& st) {
  LOG_INFO("Update");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = db->tables->at(st.table_id)->indices;

  std::string key_str = serialize(rec_ptr, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);
  off_t storage_offset;
  std::string val, before_tuple;
  record* before_rec = NULL;

  // Check if key does not exist
  if (indices->at(0)->off_map->at(key, &storage_offset) == false) {
    delete rec_ptr;
    return EXIT_SUCCESS;
  }

  val = tab->fs_data.at(storage_offset);
  before_rec = deserialize(val, tab->sptr);

  entry_stream.str("");
  entry_stream << st.transaction_id << " " << st.op_type << " " << st.table_id
               << " ";
  entry_stream << serialize(before_rec, tab->sptr) << " ";

  // Update existing record
  for (int field_itr : st.field_ids) {
    before_rec->set_data(field_itr, rec_ptr);
  }
  before_tuple = serialize(before_rec, tab->sptr);
  entry_stream << before_tuple << "\n";

  // Add log entry
  entry_str = entry_stream.str();
  fs_log.push_back(entry_str);

  // In-place update
  //LOG_INFO("update offset : %lu", storage_offset);
  tab->fs_data.update(storage_offset, before_tuple);

  delete before_rec;
  delete rec_ptr;
  return EXIT_SUCCESS;
}

void wal_engine::txn_begin() {
}

void wal_engine::txn_end(bool commit) {
}

void wal_engine::load(const statement& st) {
  //LOG_INFO("Load");
  record* after_rec = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;

  std::string key_str = serialize(after_rec, indices->at(0)->sptr);
  unsigned long key = hash_fn(key_str);

  std::string after_tuple = serialize(after_rec, after_rec->sptr);
  off_t storage_offset;

  storage_offset = tab->fs_data.push_back(after_tuple);

  // Add entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = serialize(after_rec, indices->at(index_itr)->sptr);
    key = hash_fn(key_str);

    indices->at(index_itr)->off_map->insert(key, storage_offset);
  }

  delete after_rec;
}

void wal_engine::group_commit() {

  while (ready) {
    // sync
    fs_log.sync();

    std::this_thread::sleep_for(std::chrono::milliseconds(conf.gc_interval));
  }
}

void wal_engine::recovery() {

  LOG_INFO("WAL recovery");

  // Setup recovery
  fs_log.flush();
  fs_log.sync();
  fs_log.disable();

  // Clear off_map and rebuild it
  vector<table*> tables = db->tables->get_data();
  for (table* tab : tables) {
    vector<table_index*> indices = tab->indices->get_data();

    for (table_index* index : indices) {
      index->off_map->clear();
    }
  }

  int op_type, txn_id, table_id;
  std::string entry_str, tuple_str;
  table* tab;
  statement st;
  bool undo_mode = false;

  timer rec_t;
  rec_t.start();

  std::ifstream log_file(fs_log.log_file_name);
  int total_txns = std::count(std::istreambuf_iterator<char>(log_file),
                              std::istreambuf_iterator<char>(), '\n');
  log_file.clear();
  log_file.seekg(0, ios::beg);

  while (std::getline(log_file, entry_str)) {
    //cout << "entry :  " << entry_str.c_str() << endl;
    std::stringstream entry(entry_str);

    entry >> txn_id >> op_type >> table_id;

    if (undo_mode || (total_txns - txn_id < conf.active_txn_threshold)) {
      undo_mode = true;

      switch (op_type) {
        case operation_type::Insert:
          op_type = operation_type::Delete;
          break;
        case operation_type::Delete:
          op_type = operation_type::Insert;
          break;
      }
    }

    switch (op_type) {
      case operation_type::Insert: {
        if (!undo_mode)
          LOG_INFO("Redo Insert");
          else
          LOG_INFO("Undo Delete");

        tab = db->tables->at(table_id);
        schema* sptr = tab->sptr;

        tuple_str = get_tuple(entry, sptr);
        record* after_rec = deserialize(tuple_str, sptr);
        st = statement(0, operation_type::Insert, table_id, after_rec);
        insert(st);
      }
        break;

      case operation_type::Delete: {
        if (!undo_mode)
          LOG_INFO("Redo Delete");
          else
          LOG_INFO("Undo Insert");

        tab = db->tables->at(table_id);
        schema* sptr = tab->sptr;

        tuple_str = get_tuple(entry, sptr);
        record* before_rec = deserialize(tuple_str, sptr);
        st = statement(0, operation_type::Delete, table_id, before_rec);
        remove(st);
      }
        break;

      case operation_type::Update: {
        if (!undo_mode)
          LOG_INFO("Redo Update");
          else
          LOG_INFO("Undo Update");

        tab = db->tables->at(table_id);
        schema* sptr = tab->sptr;
        tuple_str = get_tuple(entry, sptr);
        record* before_rec = deserialize(tuple_str, sptr);
        tuple_str = get_tuple(entry, sptr);
        record* after_rec = deserialize(tuple_str, sptr);

        if (!undo_mode) {
          st = statement(0, operation_type::Delete, table_id, before_rec);
          remove(st);
          st = statement(0, operation_type::Insert, table_id, after_rec);
          insert(st);
        } else {
          st = statement(0, operation_type::Delete, table_id, after_rec);
          remove(st);
          st = statement(0, operation_type::Insert, table_id, before_rec);
          insert(st);
        }
      }

        break;

      default:
        cout << "Invalid operation type" << op_type << endl;
        break;
    }

  }

  fs_log.close();

  rec_t.end();
  cout << "WAL :: Recovery duration (ms) : " << rec_t.duration() << endl;

}


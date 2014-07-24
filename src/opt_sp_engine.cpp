// OPT SP - based on PM

#include "opt_sp_engine.h"

using namespace std;

void opt_sp_engine::group_commit() {

  while (ready) {

    if (txn_ptr != NULL) {
      wrlock(&opt_sp_pbtree_rwlock);
      assert(bt->txn_commit(txn_ptr) == BT_SUCCESS);

      txn_ptr = bt->txn_begin(0);
      assert(txn_ptr);
      unlock(&opt_sp_pbtree_rwlock);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(conf.gc_interval));
  }
}

opt_sp_engine::opt_sp_engine(const config& _conf)
    : conf(_conf),
      db(conf.db),
      bt(NULL),
      txn_ptr(NULL) {

  //for (int i = 0; i < conf.num_executors; i++)
  //  executors.push_back(std::thread(&wal_engine::runner, this));

}

opt_sp_engine::~opt_sp_engine() {

  // done = true;
  //for (int i = 0; i < conf.num_executors; i++)
  //  executors[i].join();

}

std::string opt_sp_engine::select(const statement& st) {
  LOG_INFO("Select");
  record* rec_ptr = st.rec_ptr;
  record* select_ptr;
  struct cow_btval key, val;

  unsigned long key_id = hasher(hash_fn(st.key), st.table_id,
                                st.table_index_id);
  string key_str = std::to_string(key_id);
  key.data = (void*) key_str.c_str();
  key.size = key_str.size();
  std::string value;

  // Read from latest clean version
  if (bt->at(txn_ptr, &key, &val) != BT_FAIL) {
    std::sscanf((char*) val.data, "%p", &select_ptr);
    value = get_data(select_ptr, st.projection);
  }

  LOG_INFO("val : %s", value.c_str());
  //cout<<"val : " <<value<<endl;

  delete rec_ptr;

  return value;
}

void opt_sp_engine::insert(const statement& st) {
  //LOG_INFO("Insert");
  record* after_rec = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  struct cow_btval key, val;
  val.data = new char[64];

  std::string key_str = get_data(after_rec, indices->at(0)->sptr);
  unsigned long key_id = hasher(hash_fn(key_str), st.table_id, 0);
  key_str = std::to_string(key_id);

  key.data = (void*) key_str.c_str();
  key.size = key_str.size();

  // Check if key exists in current version
  if (bt->at(txn_ptr, &key, &val) != BT_FAIL) {
    delete after_rec;
    return;
  }

  // Activate new record
  pmemalloc_activate(after_rec);
  after_rec->persist_data();

  // Add entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(after_rec, indices->at(index_itr)->sptr);
    key_id = hasher(hash_fn(key_str), st.table_id, index_itr);
    key_str = std::to_string(key_id);

    key.data = (void*) key_str.c_str();
    key.size = key_str.size();
    std::sprintf((char*) val.data, "%p", after_rec);
    val.size = strlen((char*) val.data) + 1;
    bt->insert(txn_ptr, &key, &val);
  }

}

void opt_sp_engine::remove(const statement& st) {
  LOG_INFO("Remove");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  struct cow_btval key, val;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key_id = hasher(hash_fn(st.key), st.table_id, 0);
  key_str = std::to_string(key_id);

  key.data = (void*) key_str.c_str();
  key.size = key_str.size();

  // Check if key does not exist
  if (bt->at(txn_ptr, &key, &val) == BT_FAIL) {
    delete rec_ptr;
    return;
  }

  // Free record
  record* before_rec;
  std::sscanf((char*) val.data, "%p", &before_rec);

  // Remove entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(rec_ptr, indices->at(index_itr)->sptr);
    key_id = hasher(hash_fn(st.key), st.table_id, index_itr);
    key_str = std::to_string(key_id);

    key.data = (void*) key_str.c_str();
    key.size = key_str.size();

    bt->remove(txn_ptr, &key, NULL);
  }

  delete rec_ptr;
  delete before_rec;
}

void opt_sp_engine::update(const statement& st) {
  LOG_INFO("Update");
  record* rec_ptr = st.rec_ptr;
  table* tab = db->tables->at(st.table_id);
  plist<table_index*>* indices = tab->indices;

  unsigned int num_indices = tab->num_indices;
  unsigned int index_itr;
  struct cow_btval key, val, update_val;

  std::string key_str = get_data(rec_ptr, indices->at(0)->sptr);
  unsigned long key_id = hasher(hash_fn(key_str), st.table_id, 0);

  key_str = std::to_string(key_id);

  key.data = (void*) key_str.c_str();
  key.size = key_str.size();

  // Check if key does not exist in current version
  if (bt->at(txn_ptr, &key, &val) == BT_FAIL) {
    delete rec_ptr;
    return;
  }

  // Read from current version
  record* before_rec;
  std::sscanf((char*) val.data, "%p", &before_rec);

  record* after_rec = new record(before_rec->sptr);
  memcpy(after_rec->data, before_rec->data, before_rec->data_len);

  // Update record
  for (int field_itr : st.field_ids) {
    void* before_field = before_rec->get_pointer(field_itr);
    after_rec->set_data(field_itr, rec_ptr);
    delete ((char*) before_field);
  }

  // Activate new record
  pmemalloc_activate(after_rec);
  after_rec->persist_data();

  update_val.data = new char[64];
  std::sprintf((char*) update_val.data, "%p", after_rec);
  update_val.size = strlen((char*) val.data) + 1;

  // Update entry in indices
  for (index_itr = 0; index_itr < num_indices; index_itr++) {
    key_str = get_data(after_rec, indices->at(index_itr)->sptr);
    key_id = hasher(hash_fn(key_str), st.table_id, index_itr);
    key_str = std::to_string(key_id);

    key.data = (void*) key_str.c_str();
    key.size = key_str.size();

    bt->insert(txn_ptr, &key, &update_val);
  }

  //printf("before_rec :: record :: %p \n", before_rec);
  //printf("rec_ptr :: record :: %p \n", rec_ptr);

  before_rec->clear_data();
  delete rec_ptr;
}

// RUNNER + LOADER

void opt_sp_engine::execute(const transaction& txn) {

  rdlock(&opt_sp_pbtree_rwlock);

  for (const statement& st : txn.stmts) {
    if (st.op_type == operation_type::Select) {
      select(st);
    } else if (st.op_type == operation_type::Insert) {
      insert(st);
    } else if (st.op_type == operation_type::Update) {
      update(st);
    } else if (st.op_type == operation_type::Delete) {
      remove(st);
    }
  }

  unlock(&opt_sp_pbtree_rwlock);

}

void opt_sp_engine::runner() {
  bool empty = true;

  while (!done) {
    rdlock(&txn_queue_rwlock);
    empty = txn_queue.empty();
    unlock(&txn_queue_rwlock);

    if (!empty) {
      wrlock(&txn_queue_rwlock);
      const transaction& txn = txn_queue.front();
      txn_queue.pop();
      unlock(&txn_queue_rwlock);

      execute(txn);
    }
  }

  while (!txn_queue.empty()) {
    wrlock(&txn_queue_rwlock);
    const transaction& txn = txn_queue.front();
    txn_queue.pop();
    unlock(&txn_queue_rwlock);

    execute(txn);
  }
}

void opt_sp_engine::generator(const workload& load, bool stats) {

  bt = db->dirs->t_ptr;
  txn_ptr = bt->txn_begin(0);
  assert(txn_ptr);
  txn_counter = 0;
  unsigned int num_txns = load.txns.size();
  unsigned int period = ((num_txns > 10) ? (num_txns / 10) : 1);

  std::thread gc(&opt_sp_engine::group_commit, this);
  ready = true;

  struct timeval t1, t2;
  gettimeofday(&t1, NULL);

  for (const transaction& txn : load.txns) {
    execute(txn);

    if (++txn_counter % period == 0) {
      printf("Finished :: %.2lf %% \r",
             ((double) (txn_counter * 100) / num_txns));
      fflush(stdout);
    }
  }
  printf("\n");

  ready = false;
  gc.join();

  assert(bt->txn_commit(txn_ptr) == BT_SUCCESS);
  txn_ptr = NULL;

  gettimeofday(&t2, NULL);

  if (stats) {
    cout << "OPT SP :: ";
    display_stats(t1, t2, conf.num_txns);
  }
}

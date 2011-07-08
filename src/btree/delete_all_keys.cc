#include "btree/delete_all_keys.hpp"

#include "btree/leaf_node.hpp"
#include "btree/node.hpp"
#include "btree/parallel_traversal.hpp"
#include "btree/slice.hpp"
#include "buffer_cache/co_functions.hpp"

struct delete_all_keys_traversal_helper_t : public btree_traversal_helper_t {
    void preprocess_btree_superblock(UNUSED transaction_t *txn, UNUSED const btree_superblock_t *superblock) {
        // Nothing to do here, because it's for backfill.
    }

    void process_a_leaf(transaction_t *txn, buf_t *leaf_node_buf) {
        rassert(coro_t::self());
        leaf_node_t *data = reinterpret_cast<leaf_node_t *>(leaf_node_buf->get_data_major_write());

        int npairs = data->npairs;

        for (int i = 0; i < npairs; ++i) {
            uint16_t offset = data->pair_offsets[i];
            btree_leaf_pair *pair = leaf::get_pair(data, offset);

            blob_t b(reinterpret_cast<btree_value_t *>(pair->value())->value_ref(), blob::btree_maxreflen);
            b.unappend_region(txn, b.valuesize());
        }
    }

    void postprocess_internal_node(buf_t *internal_node_buf) {
        internal_node_buf->mark_deleted();
    }

    void postprocess_btree_superblock(buf_t *superblock_buf) {
        // We're deleting the entire tree, including the root node, so we need to do this.
        btree_superblock_t *superblock = reinterpret_cast<btree_superblock_t *>(superblock_buf->get_data_major_write());
        superblock->root_block = NULL_BLOCK_ID;
    }

    access_t transaction_mode() { return rwi_write; }
    access_t btree_superblock_mode() { return rwi_write; }
    access_t btree_node_mode() { return rwi_write; }

    void filter_interesting_children(UNUSED transaction_t *txn, const block_id_t *block_ids, int num_block_ids, interesting_children_callback_t *cb) {
        // There is nothing to filter, because we want to delete everything.
        boost::scoped_array<block_id_t> ids(new block_id_t[num_block_ids]);
        std::copy(block_ids, block_ids + num_block_ids, ids.get());
        cb->receive_interesting_children(ids, num_block_ids);
    }
};

// preprocess_btree_superblock above depends on the fact that this is
// for backfill.  Thus we don't have to record the fact in the delete
// queue.  (Or, we will have to, along with the neighbor that sent us
// delete-all-keys operation.)
void btree_delete_all_keys_for_backfill(btree_slice_t *slice, order_token_t token) {
    slice->assert_thread();

    rassert(coro_t::self());

    delete_all_keys_traversal_helper_t helper;

    slice->pre_begin_transaction_sink_.check_out(token);
    order_token_t begin_transaction_token = slice->pre_begin_transaction_write_mode_source_.check_in(token.tag() + "+begin_transaction_token");

    transaction_t txn(slice->cache(), helper.transaction_mode(), 0, repli_timestamp_t::invalid);

    txn.set_token(slice->post_begin_transaction_checkpoint_.check_through(token));

    // The timestamp never gets used, because we're just deleting
    // stuff.  The use of repli_timestamp_t::invalid here might trip
    // some assertions, though.
    btree_parallel_traversal(&txn, slice, &helper);
}

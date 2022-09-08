#include <torch/torch.h>
#include <torch/extension.h>

#include "quadtree.hpp"

namespace ms
{
    torch::Tensor get_block_corner_list(torch::Tensor quadtree_array, const int grid_height, const int grid_width)
    {
        std::vector<int> corner_list;
        qt_tree_t *grid_tree_cpu = quadtree_array.data_ptr<qt_tree_t>();

        for (int grid_idx = 0; grid_idx < grid_height * grid_width; ++grid_idx)
        {
            int grid_h_idx = grid_idx / grid_width;
            int grid_w_idx = grid_idx % grid_width;

            qt_tree_t *grid_tree = grid_tree_cpu + grid_idx * N_TREE_INTS;

            //index for top left corner of current block.
            int tl_x = grid_w_idx * 64;
            int tl_y = grid_h_idx * 64;

            if (tree_isset_bit(grid_tree, 0))
            {
                for (int hl1 = 0; hl1 < 2; ++hl1)
                {
                    for (int wl1 = 0; wl1 < 2; ++wl1)
                    {
                        int bit_idx_l1 = 1 + hl1 * 2 + wl1;
                        int tl_x_l1 = tl_x + wl1 * 32;
                        int tl_y_l1 = tl_y + hl1 * 32;

                        if (tree_isset_bit(grid_tree, bit_idx_l1))
                        {
                            for (int hl2 = 0; hl2 < 2; ++hl2)
                            {
                                for (int wl2 = 0; wl2 < 2; ++wl2)
                                {
                                    int bit_idx_l2 = tree_child_bit_idx(bit_idx_l1) + hl2 * 2 + wl2;
                                    int tl_x_l2 = tl_x_l1 + wl2 * 16;
                                    int tl_y_l2 = tl_y_l1 + hl2 * 16;
                                    if (tree_isset_bit(grid_tree, bit_idx_l2))
                                    {
                                        for (int hl3 = 0; hl3 < 2; ++hl3)
                                        {
                                            for (int wl3 = 0; wl3 < 2; ++wl3)
                                            {
                                                int bit_idx_l3 = tree_child_bit_idx(bit_idx_l2) + hl3 * 2 + wl3;
                                                int tl_x_l3 = tl_x_l2 + wl3 * 8;
                                                int tl_y_l3 = tl_y_l2 + hl3 * 8;
                                                if (tree_isset_bit(grid_tree, bit_idx_l3))
                                                {
                                                    for (int hl4 = 0; hl4 < 2; ++hl4)
                                                    {
                                                        for (int wl4 = 0; wl4 < 2; ++wl4)
                                                        {
                                                            int tl_x_l4 = tl_x_l3 + wl4 * 4;
                                                            int tl_y_l4 = tl_y_l3 + hl4 * 4;
                                                            corner_list.insert(corner_list.end(), {tl_y_l4, tl_x_l4, 4});
                                                        }
                                                    }
                                                }
                                                else
                                                {
                                                    corner_list.insert(corner_list.end(), {tl_y_l3, tl_x_l3, 8});
                                                }
                                            }
                                        }
                                    }
                                    else
                                    {
                                        corner_list.insert(corner_list.end(), {tl_y_l2, tl_x_l2, 16});
                                    }
                                }
                            }
                        }
                        else
                        {
                            corner_list.insert(corner_list.end(), {tl_y_l1, tl_x_l1, 32});
                        }
                    }
                }
            }
            else
            {
                corner_list.insert(corner_list.end(), {tl_y, tl_x, 64});
            }
        }

        int num_tu_blocks = corner_list.size() / 3;
        auto options = torch::TensorOptions().dtype(torch::kInt);
        auto corner_list_tensor = torch::from_blob(corner_list.data(), {num_tu_blocks, 3}, options).clone();

        return corner_list_tensor;
    }

} // namespace ms

namespace py = pybind11;

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m)
{
    m.def("get_block_corner_list", &ms::get_block_corner_list, "get_block_corner_list");
}

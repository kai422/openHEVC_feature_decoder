/**
 * @ Author: Kai Xu
 * @ Create Time: 2020-05-16 16:46:45
 * @ Modified by: Kai Xu
 * @ Modified time: 2021-01-08 22:13:30
 * @ Description:
 */
// Copyright (c) 2017, The OctNet authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the <organization> nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL OCTNET AUTHORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef QUADTREE
#define QUADTREE

#include <smmintrin.h>

typedef int qt_size_t;
typedef u_int8_t qt_tree_t;
typedef float qt_data_t;
const int N_TREE_INTS = 128;
const int N_QUAD_TREE_T_BITS = 8 * sizeof(qt_tree_t);

/// Checks if the bit on pos of num is set, or not.
///
/// @param num
/// @param pos
/// @return true, if bit is set, otherwise false
inline bool tree_isset_bit(const qt_tree_t *num, const int pos)
{
    int pos_split = pos * 2;
    return (num[pos_split / N_QUAD_TREE_T_BITS] & (1 << (pos_split % N_QUAD_TREE_T_BITS))) != 0;
}

/// Checks if the bit on pos of num is set, or not.
///
/// @param num
/// @param pos
/// @return true, if bit is set, otherwise false
inline bool tree_isset_bit_type(const qt_tree_t *num, const int pos)
{
    int pos_type = pos * 2 + 1;
    return (num[pos_type / N_QUAD_TREE_T_BITS] & (1 << (pos_type % N_QUAD_TREE_T_BITS))) != 0;
}

/// Computes the bit index of the parent for the given bit_idx.
/// Used to traverse a shallow quadtree.
/// @warning does not check the range of bit_idx, and will return an invalid
/// result if for example no parent exists (e.g. for bit_idx=0).
///
/// @param bit_idx
/// @return parent bit_idx of bit_idx
inline int tree_parent_bit_idx(const int bit_idx)
{
    return (bit_idx - 1) / 4;
}

/// Computes the bit index of the first child for the given bit_idx.
/// Used to traverse a shallow quadtree.
///
/// @param bit_idx
/// @return child bit_idx of bit_idx
inline int tree_child_bit_idx(const int bit_idx)
{
    return 4 * bit_idx + 1;
}

#endif
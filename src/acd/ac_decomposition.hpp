/* mockturtle: C++ logic network library
 * Copyright (C) 2018-2023  EPFL
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*!
  \file ac_decomposition.hpp
  \brief Ashenhurst-Curtis decomposition

  \author Alessandro Tempia Calvino
*/

#ifndef _ACD_H_
#define _ACD_H_
#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "kitty_constants.hpp"
#include "kitty_constructors.hpp"
#include "kitty_static_tt.hpp"
#include "kitty_dynamic_tt.hpp"
#include "kitty_operations.hpp"
#include "kitty_operators.hpp"

namespace mockturtle
{

/*! \brief Parameters for ac_decomposition */
struct ac_decomposition_params
{
  /*! \brief LUT size for decomposition. */
  uint32_t lut_size{ 6 };

  /*! \brief Maximum number of iterations for covering. */
  uint32_t max_iter{ 5000 };
};

/*! \brief Statistics for ac_decomposition */
struct ac_decomposition_stats
{
  uint32_t num_luts{ 0 };
  uint32_t num_edges{ 0 };
  uint32_t num_levels{ 0 };
};

struct ac_decomposition_result
{
  kitty::dynamic_truth_table tt;
  std::vector<uint32_t> support;
};

template<typename TT>
class ac_decomposition_impl
{
private:
  struct encoding_matrix
  {
    uint64_t column{ 0 };
    uint32_t cost{ 0 };
    uint32_t index{ 0 };
    uint32_t sort_cost{ 0 };
  };

private:
  static constexpr uint32_t max_num_vars = 8;
  using STT = kitty::static_truth_table<max_num_vars>;

public:
  explicit ac_decomposition_impl( TT const& tt, uint32_t num_vars, ac_decomposition_params const& ps, ac_decomposition_stats* pst = nullptr )
      : num_vars( num_vars ), ps( ps ), pst( pst ), permutations( num_vars )
  {
    tt_start = tt;
    std::iota( permutations.begin(), permutations.end(), 0 );
  }

  /*! \brief Runs ACD using late arriving variables */
  int run( unsigned delay_profile )
  {
    /* truth table is too large for the settings */
    if ( num_vars > max_num_vars )
    {
      return -1;
    }

    uint32_t late_arriving = __builtin_popcount( delay_profile );

    /* return a high cost if too many late arriving variables */
    if ( late_arriving > ps.lut_size / 2 || late_arriving > 3 )
    {
      return -1;
    }

    /* convert to static TT */
    best_tt = kitty::extend_to<max_num_vars>( tt_start );
    best_multiplicity = UINT32_MAX;
    uint32_t best_cost = UINT32_MAX;

    /* permute late arriving variables to be the least significant */
    reposition_late_arriving_variables( delay_profile, late_arriving );

    /* run ACD trying different bound sets and free sets */
    uint32_t free_set_size = late_arriving;
    uint32_t offset = std::max( static_cast<uint32_t>( late_arriving ), 1u );
    for ( uint32_t i = offset; i <= ps.lut_size / 2 && i <= 3; ++i )
    {
      auto evaluate_fn = [&]( STT const& tt ) { return column_multiplicity( tt, i ); };
      auto [tt_p, perm, cost] = enumerate_iset_combinations_offset( i, offset, evaluate_fn, false );

      /* add cost if not support reducing */
      uint32_t additional_cost = ( num_vars - i > ps.lut_size ) ? 128 : 0;
      /* check for feasible solution that improves the cost */
      if ( cost <= ( 1 << ( ps.lut_size - i ) ) && cost + additional_cost < best_cost )
      {
        best_tt = tt_p;
        permutations = perm;
        best_multiplicity = cost;
        best_cost = cost + additional_cost;
        free_set_size = i;
      }
    }

    if ( best_multiplicity == UINT32_MAX )
      return -1;

    /* compute isets */
    // std::vector<STT> isets = compute_isets( free_set_size );

    // generate_support_minimization_encodings();
    // solve_min_support_exact( isets, free_set_size );

    /* unfeasible decomposition */
    // if ( best_bound_sets.empty() )
    // {
    //   return -1;
    // }

    pst->num_luts = ps.lut_size - free_set_size;
    best_free_set = free_set_size;

    /* TODO generate decomposition only when returning the result */
    // dec_result = generate_decomposition( free_set_size );

    /* TODO: change return value */
    return 0;
  }

  int compute_decomposition()
  {
     if ( best_multiplicity == UINT32_MAX )
      return -1;

    /* compute isets */
    std::vector<STT> isets = compute_isets( best_free_set );

    generate_support_minimization_encodings();
    solve_min_support_exact( isets, best_free_set );

    /* unfeasible decomposition */
    if ( best_bound_sets.empty() )
    {
      return -1;
    }

    return 0;
  }

  unsigned get_profile()
  {
    unsigned profile = 0;

    if ( best_free_set > num_vars )
      return -1;
    
    for ( uint32_t i = 0; i < best_free_set; ++i )
    {
      profile |= 1 << permutations[i];
    }

    return profile;
  }

  std::vector<ac_decomposition_result> get_result()
  {
    return dec_result;
  }

  void get_decomposition( unsigned char *decompArray )
  {
    if ( best_free_set > num_vars )
      return;

    dec_result = generate_decomposition( best_free_set );
    return get_decomposition_abc( decompArray );
  }

private:
  uint32_t column_multiplicity( STT tt, uint32_t free_set_size )
  {
    uint64_t multiplicity_set[4] = { 0u, 0u, 0u, 0u };
    uint32_t multiplicity = 0;
    uint32_t num_blocks = ( num_vars > 6 ) ? ( 1u << ( num_vars - 6 ) ) : 1;

    /* supports up to 64 values of free set (256 for |FS| == 3)*/
    assert( free_set_size <= 3 );

    /* extract iset functions */
    if ( free_set_size == 1 )
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 32; ++j )
        {
          multiplicity_set[0] |= UINT64_C( 1 ) << ( *it & 0x3 );
          *it >>= 2;
        }
        ++it;
      }
    }
    else if ( free_set_size == 2 )
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 16; ++j )
        {
          multiplicity_set[0] |= UINT64_C( 1 ) << ( *it & 0xF );
          *it >>= 4;
        }
        ++it;
      }
    }
    else /* free set size 3 */
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 8; ++j )
        {
          multiplicity_set[( *it >> 6 ) & 0x3] |= UINT64_C( 1 ) << ( *it & 0x3F );
          *it >>= 8;
        }
        ++it;
      }
    }

    multiplicity = __builtin_popcountl( multiplicity_set[0] );

    if ( free_set_size == 3 )
    {
      multiplicity += __builtin_popcountl( multiplicity_set[1] );
      multiplicity += __builtin_popcountl( multiplicity_set[2] );
      multiplicity += __builtin_popcountl( multiplicity_set[3] );
    }

    return multiplicity;
  }

  template<typename Fn>
  std::tuple<STT, std::vector<uint32_t>, uint32_t> enumerate_iset_combinations( uint32_t free_set_size, Fn&& fn, bool verbose = false )
  {
    /* works up to 16 input truth tables */
    assert( num_vars <= 16 );

    /* special case */
    STT tt = best_tt;
    if ( num_vars <= free_set_size || free_set_size == 0 )
    {
      return { tt, permutations, UINT32_MAX };
    }

    /* select k */
    // free_set_size = std::min( free_set_size, num_vars - free_set_size );

    /* init permutation array */
    std::array<uint32_t, 16> perm, best_perm;
    std::copy( permutations.begin(), permutations.begin() + num_vars, perm.begin() );
    best_perm = perm;

    /* TT with best cost */
    STT best = tt;
    uint32_t best_cost = UINT32_MAX;

    /* enumerate combinations */
    if ( free_set_size == 1 )
    {
      uint32_t cost = fn( tt );
      if ( cost < best_cost )
      {
        best = tt;
        best_cost = cost;
        best_perm = perm;
      }

      if ( verbose )
      {
        kitty::print_hex( tt );
        std::cout << " " << cost << " ";
        print_perm( perm.begin(), free_set_size );
      }

      for ( uint32_t i = 1; i < num_vars; ++i )
      {
        std::swap( perm[0], perm[i] );
        kitty::swap_inplace( tt, 0, i );

        uint32_t cost = fn( tt );
        if ( cost < best_cost )
        {
          best = tt;
          best_cost = cost;
          best_perm = perm;
        }

        if ( verbose )
        {
          kitty::print_hex( tt );
          std::cout << " " << cost << " ";
          print_perm( perm.begin(), free_set_size );
        }
      }
    }
    else if ( free_set_size == 2 )
    {
      for ( uint32_t i = 0; i < num_vars - 1; ++i )
      {
        uint32_t cost = fn( tt );
        if ( cost < best_cost )
        {
          best = tt;
          best_cost = cost;
          best_perm = perm;
        }

        if ( verbose )
        {
          kitty::print_hex( tt );
          std::cout << " " << cost << " ";
          print_perm( perm.begin(), free_set_size );
        }

        for ( uint32_t j = 2; j < num_vars - i; ++j )
        {
          std::swap( perm[1], perm[j] );
          kitty::swap_inplace( tt, 1, j );

          uint32_t cost = fn( tt );
          if ( cost < best_cost )
          {
            best = tt;
            best_cost = cost;
            best_perm = perm;
          }

          if ( verbose )
          {
            kitty::print_hex( tt );
            std::cout << " " << cost << " ";
            print_perm( perm.begin(), free_set_size );
          }
        }

        std::swap( perm[0], perm[num_vars - i - 1] );
        kitty::swap_inplace( tt, 0, num_vars - i - 1 );
      }
    }
    else if ( free_set_size == 3 )
    {
      for ( uint32_t i = 0; i < num_vars - 2; ++i )
      {
        for ( uint32_t j = i; j < num_vars - 2; ++j )
        {
          uint32_t cost = fn( tt );
          if ( cost < best_cost )
          {
            best = tt;
            best_cost = cost;
            best_perm = perm;
          }

          if ( verbose )
          {
            kitty::print_hex( tt );
            std::cout << " " << cost << " ";
            print_perm( perm.begin(), free_set_size );
          }

          for ( uint32_t k = 3; k < num_vars - j; ++k )
          {
            std::swap( perm[2], perm[k] );
            kitty::swap_inplace( tt, 2, k );

            uint32_t cost = fn( tt );
            if ( cost < best_cost )
            {
              best = tt;
              best_cost = cost;
              best_perm = perm;
            }

            if ( verbose )
            {
              kitty::print_hex( tt );
              std::cout << " " << cost << " ";
              print_perm( perm.begin(), free_set_size );
            }
          }

          std::swap( perm[1], perm[num_vars - j - 1] );
          kitty::swap_inplace( tt, 1, num_vars - j - 1 );
        }

        std::swap( perm[0], perm[num_vars - i - 1] );
        kitty::swap_inplace( tt, 0, num_vars - i - 1 );
      }
    }

    std::vector<uint32_t> res_perm( num_vars );
    std::copy( best_perm.begin(), best_perm.begin() + num_vars, res_perm.begin() );

    return std::make_tuple( best, res_perm, best_cost );
  }

  template<typename Fn>
  std::tuple<STT, std::vector<uint32_t>, uint32_t> enumerate_iset_combinations_offset( uint32_t free_set_size, uint32_t offset, Fn&& fn, bool verbose = false )
  {
    STT tt = best_tt;

    /* TT with best cost */
    STT best_tt = tt;
    uint32_t best_cost = UINT32_MAX;

    /* works up to 16 input truth tables */
    assert( num_vars <= 16 );

    /* select k */
    free_set_size = std::min( free_set_size, num_vars - free_set_size );

    /* special case */
    if ( num_vars <= free_set_size || free_set_size <= offset )
    {
      if ( offset == free_set_size )
      {
        best_cost = fn( tt );
        if ( verbose )
        {
          kitty::print_hex( tt );
          std::cout << " " << best_cost << " ";
          print_perm( permutations.begin(), free_set_size );
        }

        return { tt, permutations, best_cost };
      }
      else
      {
        return { tt, permutations, UINT32_MAX };
      }
    }

    /* decrease combinations */
    free_set_size -= offset;

    /* init permutation array */
    std::array<uint32_t, 16> perm, best_perm;
    std::copy( permutations.begin(), permutations.begin() + num_vars, perm.begin() );
    best_perm = perm;

    /* enumerate combinations */
    if ( free_set_size == 1 )
    {
      uint32_t cost = fn( tt );
      if ( cost < best_cost )
      {
        best_tt = tt;
        best_cost = cost;
        best_perm = perm;
      }

      if ( verbose )
      {
        kitty::print_hex( tt );
        std::cout << " " << cost << " ";
        print_perm( perm.begin(), free_set_size + offset );
      }

      for ( uint32_t i = offset + 1; i < num_vars; ++i )
      {
        std::swap( perm[offset], perm[i] );
        kitty::swap_inplace( tt, offset, i );

        uint32_t cost = fn( tt );
        if ( cost < best_cost )
        {
          best_tt = tt;
          best_cost = cost;
          best_perm = perm;
        }

        if ( verbose )
        {
          kitty::print_hex( tt );
          std::cout << " " << cost << " ";
          print_perm( perm.begin(), free_set_size + offset );
        }
      }
    }
    else if ( free_set_size == 2 )
    {
      for ( uint32_t i = 0; i < num_vars - 1 - offset; ++i )
      {
        uint32_t cost = fn( tt );
        if ( cost < best_cost )
        {
          best_tt = tt;
          best_cost = cost;
          best_perm = perm;
        }

        if ( verbose )
        {
          kitty::print_hex( tt );
          std::cout << " " << cost << " ";
          print_perm( perm.begin(), free_set_size + offset );
        }

        for ( uint32_t j = offset + 2; j < num_vars - i; ++j )
        {
          std::swap( perm[offset + 1], perm[j] );
          kitty::swap_inplace( tt, offset + 1, j );

          uint32_t cost = fn( tt );
          if ( cost < best_cost )
          {
            best_tt = tt;
            best_cost = cost;
            best_perm = perm;
          }

          if ( verbose )
          {
            kitty::print_hex( tt );
            std::cout << " " << cost << " ";
            print_perm( perm.begin(), free_set_size + offset );
          }
        }

        std::swap( perm[offset], perm[num_vars - i - 1] );
        kitty::swap_inplace( tt, offset, num_vars - i - 1 );
      }
    }
    else if ( free_set_size == 3 )
    {
      for ( uint32_t i = 0; i < num_vars - 2 - offset; ++i )
      {
        for ( uint32_t j = i; j < num_vars - 2 - offset; ++j )
        {
          uint32_t cost = fn( tt );
          if ( cost < best_cost )
          {
            best_tt = tt;
            best_cost = cost;
            best_perm = perm;
          }

          if ( verbose )
          {
            kitty::print_hex( tt );
            std::cout << " " << cost << " ";
            print_perm( perm.begin(), free_set_size + offset );
          }

          for ( uint32_t k = offset + 3; k < num_vars - j; ++k )
          {
            std::swap( perm[offset + 2], perm[k] );
            kitty::swap_inplace( tt, offset + 2, k );

            uint32_t cost = fn( tt );
            if ( cost < best_cost )
            {
              best_tt = tt;
              best_cost = cost;
              best_perm = perm;
            }

            if ( verbose )
            {
              kitty::print_hex( tt );
              std::cout << " " << cost << " ";
              print_perm( perm.begin(), free_set_size + offset );
            }
          }

          std::swap( perm[offset + 1], perm[num_vars - j - 1] );
          kitty::swap_inplace( tt, offset + 1, num_vars - j - 1 );
        }

        std::swap( perm[offset], perm[num_vars - i - 1] );
        kitty::swap_inplace( tt, offset, num_vars - i - 1 );
      }
    }

    std::vector<uint32_t> res_perm( num_vars );
    std::copy( best_perm.begin(), best_perm.begin() + num_vars, res_perm.begin() );

    return std::make_tuple( best_tt, res_perm, best_cost );
  }

  std::vector<STT> compute_isets( uint32_t free_set_size, bool verbose = false )
  {
    /* construct isets involved in multiplicity */
    uint32_t isets_support = num_vars - free_set_size;
    std::vector<STT> isets( best_multiplicity );

    /* construct isets */
    std::unordered_map<uint64_t, uint32_t> column_to_iset;
    STT tt = best_tt;
    uint32_t offset = 0;
    uint32_t num_blocks = ( num_vars > 6 ) ? ( 1u << ( num_vars - 6 ) ) : 1;

    if ( free_set_size == 1 )
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 32; ++j )
        {
          uint64_t val = *it & 0x3;

          if ( auto el = column_to_iset.find( val ); el != column_to_iset.end() )
          {
            isets[el->second]._bits[i / 2] |= UINT64_C( 1 ) << ( j + offset );
          }
          else
          {
            isets[column_to_iset.size()]._bits[i / 2] |= UINT64_C( 1 ) << ( j + offset );
            column_to_iset[val] = column_to_iset.size();
          }

          *it >>= 2;
        }

        offset ^= 32;
        ++it;
      }
    }
    else if ( free_set_size == 2 )
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 16; ++j )
        {
          uint64_t val = *it & 0xF;

          if ( auto el = column_to_iset.find( val ); el != column_to_iset.end() )
          {
            isets[el->second]._bits[i / 4] |= UINT64_C( 1 ) << ( j + offset );
          }
          else
          {
            isets[column_to_iset.size()]._bits[i / 4] |= UINT64_C( 1 ) << ( j + offset );
            column_to_iset[val] = column_to_iset.size();
          }

          *it >>= 4;
        }

        offset = ( offset + 16 ) % 64;
        ++it;
      }
    }
    else /* free set size 3 */
    {
      auto it = std::begin( tt );
      for ( auto i = 0u; i < num_blocks; ++i )
      {
        for ( auto j = 0; j < 8; ++j )
        {
          uint64_t val = *it & 0xFF;

          if ( auto el = column_to_iset.find( val ); el != column_to_iset.end() )
          {
            isets[el->second]._bits[i / 8] |= UINT64_C( 1 ) << ( j + offset );
          }
          else
          {
            isets[column_to_iset.size()]._bits[i / 8] |= UINT64_C( 1 ) << ( j + offset );
            column_to_iset[val] = column_to_iset.size();
          }

          *it >>= 8;
        }

        offset = ( offset + 8 ) % 64;
        ++it;
      }
    }

    /* extend isets to cover the whole truth table */
    for ( STT& iset : isets )
    {
      local_extend_to( iset, isets_support );
    }

    /* save free_set functions */
    std::vector<STT> free_set_tts( best_multiplicity );

    /* TODO: possible conflict */
    for ( auto const& pair : column_to_iset )
    {
      free_set_tts[pair.second]._bits[0] = pair.first;
      local_extend_to( free_set_tts[pair.second], free_set_size );
    }

    /* print isets  and free set*/
    if ( verbose )
    {
      std::cout << "iSets\n";
      uint32_t i = 0;
      for ( auto iset : isets )
      {
        kitty::print_hex( iset );
        std::cout << " of func ";
        kitty::print_hex( free_set_tts[i++] );
        std::cout << "\n";
      }
    }

    best_free_set_tts = std::move( free_set_tts );

    return isets;
  }

  std::vector<ac_decomposition_result> generate_decomposition( uint32_t free_set_size )
  {
    std::vector<ac_decomposition_result> res;

    for ( uint32_t i = 0; i < best_bound_sets.size(); ++i )
    {
      ac_decomposition_result dec;
      auto tt = best_bound_sets[i];
      auto care = best_care_sets[i];

      /* compute and minimize support for bound set variables */
      uint32_t k = 0;
      for ( uint32_t j = 0; j < num_vars - free_set_size; ++j )
      {
        if ( !kitty::has_var( tt, j ) )
          continue;

        if ( !kitty::has_var( tt, care, j ) )
        {
          /* fix truth table */
          adjust_truth_table_on_dc( tt, care, j );
          continue;
        }

        if ( k < j )
        {
          kitty::swap_inplace( tt, k, j );
          kitty::swap_inplace( care, k, j );
        }
        dec.support.push_back( permutations[free_set_size + j] );
        ++k;
      }

      dec.tt = kitty::shrink_to( tt, dec.support.size() );
      res.push_back( dec );
    }

    /* compute the decomposition for the top-level LUT */
    compute_top_lut_decomposition( res, free_set_size );

    return res;
  }

  void compute_top_lut_decomposition( std::vector<ac_decomposition_result>& res, uint32_t free_set_size )
  {
    uint32_t top_vars = best_bound_sets.size() + free_set_size;
    assert( top_vars <= ps.lut_size );

    /* extend bound set functions with free_set_size LSB vars */
    kitty::dynamic_truth_table tt( top_vars );

    /* compute support */
    res.emplace_back();
    for ( uint32_t i = 0; i < free_set_size; ++i )
    {
      res.back().support.push_back( permutations[i] );
    }

    /* create functions for bound set */
    std::vector<kitty::dynamic_truth_table> bound_set_vars;
    auto res_it = res.begin();
    uint32_t offset = 0;
    for ( uint32_t i = 0; i < best_bound_sets.size(); ++i )
    {
      bound_set_vars.emplace_back( top_vars );
      kitty::create_nth_var( bound_set_vars[i], free_set_size + i );

      /* add bound-set variables to the support, remove buffers */
      if ( res_it->support.size() == 1 )
      {
        res.back().support.push_back( res_it->support.front() );
        /* it is a NOT */
        if ( ( res_it->tt._bits[0] & 1 ) == 1 )
        {
          bound_set_vars[i] = ~bound_set_vars[i];
        }
        res.erase( res_it );
        ++offset;
      }
      else
      {
        res.back().support.push_back( num_vars + i - offset );
        ++res_it;
      }
    }

    /* create composition function */
    for ( uint32_t i = 0; i < best_free_set_tts.size(); ++i )
    {
      kitty::dynamic_truth_table free_set_tt = kitty::shrink_to( best_free_set_tts[i], top_vars );

      /* find MUX assignments */
      for ( uint32_t j = 0; j < bound_set_vars.size(); ++j )
      {
        /* AND with ONSET or OFFSET */
        if ( ( ( best_iset_onset[j] >> i ) & 1 ) )
        {
          free_set_tt &= bound_set_vars[j];
        }
        else if ( ( ( best_iset_offset[j] >> i ) & 1 ) )
        {
          free_set_tt &= ~bound_set_vars[j];
        }
      }

      tt |= free_set_tt;
    }

    /* add top-level LUT to result */
    res.back().tt = tt;
  }

  inline void reposition_late_arriving_variables( unsigned delay_profile, uint32_t late_arriving )
  {
    uint32_t k = 0;
    for ( uint32_t i = 0; i < late_arriving; ++i )
    {
      while ( ( ( delay_profile >> k ) & 1 ) == 0 )
        ++k;

      if ( permutations[i] == k )
      {
        ++k;
        continue;
      }

      std::swap( permutations[i], permutations[k] );
      kitty::swap_inplace( best_tt, i, k );
      ++k;
    }
  }

  template<class Iterator>
  void print_perm( Iterator begin, uint32_t free_set )
  {
    std::cout << "[";
    for ( uint32_t i = 0; i < num_vars; ++i )
    {
      if ( i == free_set )
      {
        std::cout << ", ";
      }
      std::cout << *begin << " ";
      ++begin;
    }
    std::cout << "]\n";
  }

  void generate_support_minimization_encodings()
  {
    uint32_t count = 0;
    uint32_t num_combs_exact[4] = { 2, 6, 70, 12870 };

    /* enable don't cares only if not a power of 2 */
    uint32_t num_combs = 3;
    if ( __builtin_popcount( best_multiplicity ) == 1 )
    {
      for ( uint32_t i = 0; i < 4; ++i )
      {
        if ( ( best_multiplicity >> i ) == 2u )
        {
          num_combs = num_combs_exact[i];
        }
      }
      support_minimization_encodings = std::vector<std::array<uint32_t, 2>>( num_combs );
      generate_support_minimization_encodings_rec<false>( 0, 0, 0, count );
    }
    else
    {
      for ( uint32_t i = 1; i < best_multiplicity; ++i )
      {
        num_combs = ( num_combs << 1 ) + num_combs;
      }
      support_minimization_encodings = std::vector<std::array<uint32_t, 2>>( num_combs );
      generate_support_minimization_encodings_rec<true>( 0, 0, 0, count );
    }

    assert( count == num_combs );

    /* print combinations */
    // std::cout << "{ ";
    // for ( auto const& entry : support_minimization_encodings )
    // {
    //   std::cout << "{ " << entry[0] << ", " << entry[1] << " }, ";
    // }
    // std::cout << "}\n";
  }

  template<bool enable_dcset>
  void generate_support_minimization_encodings_rec( uint64_t onset, uint64_t offset, uint32_t var, uint32_t& count )
  {
    if ( var == best_multiplicity )
    {
      if constexpr ( !enable_dcset )
      {
        /* sets must be equally populated */
        if ( __builtin_popcountl( onset ) != __builtin_popcountl( offset ) )
        {
          return;
        }
      }

      support_minimization_encodings[count][0] = onset;
      support_minimization_encodings[count][1] = offset;
      ++count;
      return;
    }

    /* move var in DCSET */
    if constexpr ( enable_dcset )
    {
      generate_support_minimization_encodings_rec<enable_dcset>( onset, offset, var + 1, count );
    }

    /* move var in ONSET */
    onset |= 1 << var;
    generate_support_minimization_encodings_rec<enable_dcset>( onset, offset, var + 1, count );
    onset &= ~( 1 << var );

    /* move var in OFFSET */
    offset |= 1 << var;
    generate_support_minimization_encodings_rec<enable_dcset>( onset, offset, var + 1, count );
    offset &= ~( 1 << var );
  }

  void solve_min_support_exact( std::vector<STT> const& isets, uint32_t free_set_size )
  {
    std::vector<encoding_matrix> matrix;
    matrix.reserve( support_minimization_encodings.size() );
    best_bound_sets.clear();

    /* create covering matrix */
    if ( !create_covering_matrix( isets, matrix, free_set_size, best_multiplicity > 4 ) )
    {
      return;
    }

    /* solve the covering problem */
    std::array<uint32_t, 5> solution = covering_solve_exact<true>( matrix, 100, ps.max_iter );

    /* check for failed decomposition */
    if ( solution[0] == UINT32_MAX )
    {
      return;
    }

    /* compute best bound sets */
    uint32_t num_luts = 1 + solution[4];
    uint32_t num_levels = 2;
    uint32_t num_edges = free_set_size + solution[4];
    uint32_t isets_support = num_vars - free_set_size;
    best_care_sets.clear();
    best_iset_onset.clear();
    best_iset_offset.clear();
    for ( uint32_t i = 0; i < solution[4]; ++i )
    {
      STT tt;
      STT care;

      const uint32_t onset = support_minimization_encodings[matrix[solution[i]].index][0];
      const uint32_t offset = support_minimization_encodings[matrix[solution[i]].index][1];
      for ( uint32_t j = 0; j < best_multiplicity; ++j )
      {
        if ( ( ( onset >> j ) & 1 ) )
        {
          tt |= isets[j];
        }
        if ( ( ( offset >> j ) & 1 ) )
        {
          care |= isets[j];
        }
      }

      care |= tt;
      num_edges += matrix[solution[i]].cost & ( ( 1 << isets_support ) - 1 );

      best_bound_sets.push_back( tt );
      best_care_sets.push_back( care );
      best_iset_onset.push_back( onset );
      best_iset_offset.push_back( offset );
    }

    if ( pst != nullptr )
    {
      pst->num_luts = num_luts;
      pst->num_levels = num_levels;
      pst->num_edges = num_edges;
    }
  }

  bool create_covering_matrix( std::vector<STT> const& isets, std::vector<encoding_matrix>& matrix, uint32_t free_set_size, bool sort )
  {
    assert( best_multiplicity < 12 );
    uint32_t combinations = ( best_multiplicity * ( best_multiplicity - 1 ) ) / 2;
    uint64_t sol_existance = 0;
    uint32_t iset_support = num_vars - free_set_size;

    /* insert dichotomies */
    for ( uint32_t i = 0; i < support_minimization_encodings.size(); ++i )
    {
      uint32_t const onset = support_minimization_encodings[i][0];
      uint32_t const offset = support_minimization_encodings[i][1];

      uint32_t ones_onset = __builtin_popcount( onset );
      uint32_t ones_offset = __builtin_popcount( offset );

      /* filter columns that do not distinguish pairs */
      if ( ones_onset == 0 || ones_offset == 0 || ones_onset == best_multiplicity || ones_offset == best_multiplicity )
      {
        continue;
      }

      /* compute function and distinguishable seed dichotomies */
      uint64_t column = 0;
      STT tt;
      STT care;
      uint32_t pair_pointer = 0;
      for ( uint32_t j = 0; j < best_multiplicity; ++j )
      {
        auto onset_shift = ( onset >> j );
        auto offset_shift = ( offset >> j );
        if ( ( onset_shift & 1 ) )
        {
          tt |= isets[j];
        }

        if ( ( offset_shift & 1 ) )
        {
          care |= isets[j];
        }

        /* compute included seed dichotomies */
        for ( uint32_t k = j + 1; k < best_multiplicity; ++k )
        {
          /* if is are in diffent sets */
          if ( ( ( ( onset_shift & ( offset >> k ) ) | ( ( onset >> k ) & offset_shift ) ) & 1 ) )
          {
            column |= UINT64_C( 1 ) << ( pair_pointer );
          }

          ++pair_pointer;
        }
      }

      care |= tt;

      /* compute cost */
      uint32_t cost = 0;
      for ( uint32_t j = 0; j < iset_support; ++j )
      {
        cost += has_var_support( tt, care, iset_support, j ) ? 1 : 0;
      }

      /* discard solutions with support over LUT size */
      if ( cost > ps.lut_size )
        continue;

      if ( cost > 1 )
      {
        cost |= 1 << iset_support;
      }

      uint32_t sort_cost = cost + ( ( combinations - __builtin_popcountl( column ) ) << num_vars );

      /* insert */
      matrix.emplace_back( encoding_matrix{ column, cost, i, sort_cost } );

      sol_existance |= column;
    }

    /* necessary condition for the existance of a solution */
    if ( __builtin_popcountl( sol_existance ) != combinations )
    {
      return false;
    }

    if ( !sort )
    {
      return true;
    }

    std::sort( matrix.begin(), matrix.end(), [&]( auto const& a, auto const& b ) {
      return a.sort_cost < b.sort_cost;
    } );

    /* print */
    // if ( best_multiplicity < 6 )
    // {
    //   for ( uint32_t i = 0; i < columns.size(); ++i )
    //   {
    //     std::cout << indexes[i] << " " << costs[i] << " \t" << columns[i] << "\n";
    //   }
    // }

    return true;
  }

  template<bool limit_iter = false>
  std::array<uint32_t, 5> covering_solve_exact( std::vector<encoding_matrix>& matrix, uint32_t max_iter = 100, int32_t limit = 2000 )
  {
    /* last value of res contains the size of the bound set */
    std::array<uint32_t, 5> res = { UINT32_MAX };
    uint32_t best_cost = UINT32_MAX;
    uint32_t combinations = ( best_multiplicity * ( best_multiplicity - 1 ) ) / 2;
    bool looping = true;

    assert( best_multiplicity <= 16 );

    /* determine the number of needed loops*/
    if ( best_multiplicity <= 4 )
    {
      res[4] = 2;
      for ( uint32_t i = 0; i < matrix.size() - 1; ++i )
      {
        for ( uint32_t j = 1; j < matrix.size(); ++j )
        {
          /* filter by cost */
          if ( matrix[i].cost + matrix[j].cost >= best_cost )
            continue;

          /* check validity */
          if ( __builtin_popcountl( matrix[i].column | matrix[j].column ) == combinations )
          {
            res[0] = i;
            res[1] = j;
            best_cost = matrix[i].cost + matrix[j].cost;
          }
        }
      }
    }
    else if ( best_multiplicity <= 8 )
    {
      res[4] = 3;
      for ( uint32_t i = 0; i < matrix.size() - 2 && looping; ++i )
      {
        /* limit */
        if constexpr ( limit_iter )
        {
          if ( limit <= 0 || ( best_cost < UINT32_MAX && max_iter == 0 ) )
          {
            looping = false;
          }
        }

        for ( uint32_t j = 1; j < matrix.size() - 1 && looping; ++j )
        {
          uint64_t current_columns = matrix[i].column | matrix[j].column;
          uint32_t current_cost = matrix[i].cost + matrix[j].cost;

          /* limit */
          if constexpr ( limit_iter )
          {
            if ( limit <= 0 || ( best_cost < UINT32_MAX && max_iter == 0 ) )
            {
              looping = false;
            }
          }

          /* bound */
          if ( current_cost >= best_cost )
          {
            continue;
          }

          for ( uint32_t k = 2; k < matrix.size() && looping; ++k )
          {
            /* limit */
            if constexpr ( limit_iter )
            {
              if ( limit-- <= 0 || ( best_cost < UINT32_MAX && max_iter-- == 0 ) )
              {
                looping = false;
              }
            }

            /* filter by cost */
            if ( current_cost + matrix[k].cost >= best_cost )
              continue;

            /* check validity */
            if ( __builtin_popcountl( current_columns | matrix[k].column ) == combinations )
            {
              res[0] = i;
              res[1] = j;
              res[2] = k;
              best_cost = current_cost + matrix[k].cost;
            }
          }
        }
      }
    }
    else
    {
      res[4] = 4;
      for ( uint32_t i = 0; i < matrix.size() - 3 && looping; ++i )
      {
        /* limit */
        if constexpr ( limit_iter )
        {
          if ( limit <= 0 || ( best_cost < UINT32_MAX && max_iter == 0 ) )
          {
            looping = false;
          }
        }

        for ( uint32_t j = 1; j < matrix.size() - 2 && looping; ++j )
        {
          uint64_t current_columns0 = matrix[i].column | matrix[j].column;
          uint32_t current_cost0 = matrix[i].cost + matrix[j].cost;

          /* limit */
          if constexpr ( limit_iter )
          {
            if ( limit <= 0 || ( best_cost < UINT32_MAX && max_iter == 0 ) )
            {
              looping = false;
            }
          }

          /* bound */
          if ( current_cost0 >= best_cost )
          {
            continue;
          }

          for ( uint32_t k = 2; k < matrix.size() - 1 && looping; ++k )
          {
            uint64_t current_columns1 = current_columns0 | matrix[k].column;
            uint32_t current_cost1 = current_cost0 + matrix[k].cost;

            /* limit */
            if constexpr ( limit_iter )
            {
              if ( limit <= 0 || ( best_cost < UINT32_MAX && max_iter == 0 ) )
              {
                looping = false;
              }
            }

            /* bound */
            if ( current_cost1 >= best_cost )
            {
              continue;
            }

            for ( uint32_t t = 3; t < matrix.size() && looping; ++t )
            {
              /* limit */
              if constexpr ( limit_iter )
              {
                if ( limit-- <= 0 || ( best_cost < UINT32_MAX && max_iter-- == 0 ) )
                {
                  looping = false;
                }
              }

              /* filter by cost */
              if ( current_cost1 + matrix[t].cost >= best_cost )
                continue;

              /* check validity */
              if ( __builtin_popcountl( current_columns1 | matrix[t].column ) == combinations )
              {
                res[0] = i;
                res[1] = j;
                res[2] = k;
                res[3] = t;
                best_cost = current_cost1 + matrix[t].cost;
              }
            }
          }
        }
      }
    }

    return res;
  }

  void adjust_truth_table_on_dc( STT& tt, STT& care, uint32_t var_index )
  {
    assert( var_index < tt.num_vars() );
    assert( tt.num_vars() == care.num_vars() );

    if ( tt.num_vars() <= 6 || var_index < 6 )
    {
      auto it_tt = std::begin( tt._bits );
      auto it_care = std::begin( care._bits );
      while ( it_tt != std::end( tt._bits ) )
      {
        uint64_t new_bits = *it_tt & *it_care;
        *it_tt = ( ( new_bits | ( new_bits >> ( uint64_t( 1 ) << var_index ) ) ) & kitty::detail::projections_neg[var_index] ) |
                 ( ( new_bits | ( new_bits << ( uint64_t( 1 ) << var_index ) ) ) & kitty::detail::projections[var_index] );
        *it_care = *it_care | ( *it_care >> ( uint64_t( 1 ) << var_index ) );

        ++it_tt;
        ++it_care;
      }
      return;
    }

    const auto step = 1 << ( var_index - 6 );
    for ( auto i = 0u; i < static_cast<uint32_t>( tt.num_blocks() ); i += 2 * step )
    {
      for ( auto j = 0; j < step; ++j )
      {
        tt._bits[i + j] = ( tt._bits[i + j] & care._bits[i + j] ) | ( tt._bits[i + j + step] & care._bits[i + j + step] );
        tt._bits[i + j + step] = tt._bits[i + j];
        care._bits[i + j] = care._bits[i + j] | care._bits[i + j + step];
        care._bits[i + j + step] = care._bits[i + j];
      }
    }
  }

  void local_extend_to( STT& tt, uint32_t real_num_vars )
  {
    if ( real_num_vars < 6 )
    {
      auto mask = *tt.begin();

      for ( auto i = real_num_vars; i < num_vars; ++i )
      {
        mask |= ( mask << ( 1 << i ) );
      }

      std::fill( tt.begin(), tt.end(), mask );
    }
    else
    {
      uint32_t num_blocks = ( 1u << ( real_num_vars - 6 ) );
      auto it = tt.begin();
      while ( it != tt.end() )
      {
        it = std::copy( tt.cbegin(), tt.cbegin() + num_blocks, it );
      }
    }
  }

  bool has_var_support( const STT& tt, const STT& care, uint32_t real_num_vars, uint8_t var_index )
  {
    assert( var_index < real_num_vars );
    assert( real_num_vars <= tt.num_vars() );
    assert( tt.num_vars() == care.num_vars() );

    const uint32_t num_blocks = real_num_vars <= 6 ? 1 : ( 1 << ( real_num_vars - 6 ) );
    if ( real_num_vars <= 6 || var_index < 6 )
    {
      auto it_tt = std::begin( tt._bits );
      auto it_care = std::begin( care._bits );
      while ( it_tt != std::begin( tt._bits ) + num_blocks )
      {
        if ( ( ( ( *it_tt >> ( uint64_t( 1 ) << var_index ) ) ^ *it_tt ) & kitty::detail::projections_neg[var_index]
            & ( *it_care >> ( uint64_t( 1 ) << var_index ) ) & *it_care ) != 0 )
        {
          return true;
        }
        ++it_tt;
        ++it_care;
      }

      return false;
    }

    const auto step = 1 << ( var_index - 6 );
    for ( auto i = 0u; i < num_blocks; i += 2 * step )
    {
      for ( auto j = 0; j < step; ++j )
      {
        if ( ( ( tt._bits[i + j] ^ tt._bits[i + j + step] ) & care._bits[i + j] & care._bits[i + j + step] ) != 0 )
        {
          return true;
        }
      }
    }

    return false;
  }

  void get_decomposition_abc( unsigned char *decompArray )
  {
    unsigned char *pArray = decompArray;
    unsigned char bytes = 2;

    /* write number of LUTs */
    pArray++;
    *pArray = dec_result.size();
    pArray++;

    /* write LUTs */
    for ( ac_decomposition_result const& lut : dec_result )
    {
      /* write fanin size*/
      *pArray = lut.support.size();
      pArray++; ++bytes;

      /* write support */
      for ( uint32_t i : lut.support )
      {
        *pArray = (unsigned char) i;
        pArray++; ++bytes;
      }

      /* write truth table */
      uint32_t tt_num_bytes = ( lut.tt.num_vars() <= 3 ) ? 1 : ( 1 << ( lut.tt.num_vars() - 3 ) );
      tt_num_bytes = std::min( tt_num_bytes, 8u );
      for ( uint32_t i = 0; i < lut.tt.num_blocks(); ++i )
      {
        for ( uint32_t j = 0; j < tt_num_bytes; ++j )
        {
          *pArray = (unsigned char) ( ( lut.tt._bits[i] >> ( 8 * j ) ) & 0xFF );
          pArray++; ++bytes;
        }
      }
    }

    /* write numBytes */
    *decompArray = bytes;
  }

private:
  uint32_t best_multiplicity{ UINT32_MAX };
  uint32_t best_free_set{ UINT32_MAX };
  STT best_tt;
  std::vector<STT> best_bound_sets;
  std::vector<STT> best_care_sets;
  std::vector<STT> best_free_set_tts;
  std::vector<uint64_t> best_iset_onset;
  std::vector<uint64_t> best_iset_offset;
  std::vector<ac_decomposition_result> dec_result;

  std::vector<std::array<uint32_t, 2>> support_minimization_encodings;

  TT tt_start;
  uint32_t num_vars;
  ac_decomposition_params const& ps;
  ac_decomposition_stats* pst;
  std::vector<uint32_t> permutations;
};

} // namespace mockturtle

#endif // _ACD_H_
/*
 * Copyright (c) 2014-2017 Reed A. Cartwright
 * Copyright (c) 2015 Kael Dai
 * Authors:  Reed A. Cartwright <reed@cartwrig.ht>
 *           Kael Dai <kdai1@asu.edu>
 *
 * This file is part of DeNovoGear.
 *
 * DeNovoGear is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <string>
#include <array>
#include <climits>
#include <limits>

#include <boost/range/iterator_range_core.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/range/algorithm/stable_sort.hpp>
#include <boost/range/algorithm/for_each.hpp>

#include <dng/task/pileup.h>
#include <dng/io/fasta.h>
#include <dng/io/ad.h>
#include <dng/io/bam.h>
#include <dng/seq.h>
#include <dng/utility.h>
#include <dng/depths.h>
#include <dng/matrix.h>

#include "version.h"

using namespace std;
using namespace dng;
using namespace dng::task;

// Sub-tasks
int process_bam(Pileup::argument_type &arg);
int process_ad(Pileup::argument_type &arg);

// The main loop for dng-pileup application
// argument_type arg holds the processed command line arguments
int Pileup::operator()(Pileup::argument_type &arg) {
    using utility::FileCat;
    using utility::FileCatSet;
    // if input is empty default to stdin.
    if(arg.input.empty()) {
        arg.input.emplace_back("-");
    }

    // Check that all input formats are of same category
    auto it = arg.input.begin();
    FileCat mode = utility::input_category(*it, FileCat::Sequence|FileCat::Pileup, FileCat::Sequence);
    for(++it; it != arg.input.end(); ++it) {
        if(utility::input_category(*it, FileCat::Sequence|FileCat::Pileup, FileCat::Sequence) != mode) {
            throw runtime_error("Argument error: mixing sam/bam/cram and tad/ad input files is not supported.");
        }
    }
    // Execute sub tasks based on input type
    if(mode == FileCat::Pileup) {
        return process_ad(arg);
    }
    return process_bam(arg);
}

// The main loop for dng-pileup application
// argument_type arg holds the processed command line arguments
int process_bam(Pileup::argument_type &arg) {
    using dng::pileup::AlleleDepths;
    using dng::io::BamPileup;

    // Open Reference
    if(arg.fasta.empty()){
    	throw std::invalid_argument("Path to reference file must be specified with --fasta when processing bam/sam/cram files.");
    }
    io::Fasta reference{arg.fasta.c_str()};

    // Open input files
    BamPileup mpileup{arg.min_qlen, arg.rgtag};
    for(auto && str : arg.input) {
        // TODO: We put all this logic here to simplify the BamPileup construction
        // TODO: However it might make sense to incorporate it into BamPileup::AddFile
        hts::bam::File input{str.c_str(), "r", arg.fasta.c_str(), arg.min_mapqual, arg.header.c_str()};
        if(!input.is_open()) {
            throw std::runtime_error("ERROR: unable to open bam/sam/cram input file '" + str + "' for reading.");
        }
        // add regions
        if(!arg.region.empty()) {
            if(arg.region.find(".bed") != std::string::npos) {
                input.regions(regions::bam_parse_bed(arg.region, input));
            } else {
                input.regions(regions::bam_parse_region(arg.region, input));
            }
        }
        mpileup.AddFile(std::move(input));
    }

    // Pileup data
    dng::pileup::RawDepths read_depths(mpileup.num_libraries());

    const int min_basequal = arg.min_basequal;
    auto filter_read = [min_basequal](
    BamPileup::data_type::value_type::const_reference r) -> bool {
        return (r.is_missing
        || r.qual.first[r.pos] < min_basequal
        || seq::base_index(r.aln.seq_at(r.pos)) >= 4);
    };

    // Outputfile
    io::Ad output{arg.output, std::ios_base::out};
    if(!output) {
        throw std::runtime_error("Argument Error: unable to open output file '" + arg.output + "'.");
    }

    string dict = reference.FetchDictionary();
    if(dict.empty()) {
        throw std::runtime_error("Error: Dictionary for reference " + reference.path() + " is missing.");
    }
    output.AddHeaderLines(dict);

    // Construct Library Information
    for(size_t u=0; u < mpileup.num_libraries(); ++u) {
        string attr;
        for(auto &&a : mpileup.read_groups(u)) {
            if(!attr.empty()) {
                attr += '\t';
            }
            attr += "RG:";
            attr += a;
        }
        output.AddHeaderLibrary(mpileup.libraries().names[u], mpileup.libraries().samples[u],
            std::move(attr));
    }

    if(arg.body_only && output.format() == io::Ad::Format::AD) {
        throw runtime_error("Argument Error: -B / --body-only not supported with binary output (.ad).");
    }
    if(arg.header_only && output.format() == io::Ad::Format::AD) {
        throw runtime_error("Argument Error: -H / --header-only not supported with binary output (.ad).");
    }
    if(!arg.body_only) {
        output.WriteHeader();
    }
    if(arg.header_only) {
        return EXIT_SUCCESS;
    }

    // setup initial line buffer
    AlleleDepths line;
    line.resize(63,mpileup.num_libraries());

    // Do pileup
    const bam_hdr_t *h = mpileup.header();
    assert(h != nullptr);
    if(h->n_targets != output.contigs().size()) {
        throw runtime_error("Different number of @SQ lines in reference dictionary and first sam/bam/cram file.");
    }
    for(int n = 0; n < h->n_targets; ++n) {
        if(output.contig(n).name != h->target_name[n] ||
           output.contig(n).length != h->target_len[n]) {
            throw runtime_error("Disagreement between @SQ lines in reference dictionary and first sam/bam/cram file." );
        }
    }

    // calculate maximum and minimum depths
    int min_dp = std::max(arg.min_dp,1);
    int max_dp = (arg.max_dp > 0) ? arg.max_dp : std::numeric_limits<int>::max();
    if(max_dp < min_dp) {
        throw runtime_error("Argument error: Calculated maximum depth '" + to_string(max_dp) +
            "' is less than calculated min depth '" + to_string(min_dp) + "'.");
    }
    mpileup([&,h](const BamPileup::data_type & data, utility::location_t loc) {
        // Calculate target position and fetch sequence name
        int contig = utility::location_to_contig(loc);
        int pos = utility::location_to_position(loc);
        assert(0 <= contig && contig < h->n_targets);
        char ref_base = reference.FetchBase(h->target_name[contig],pos);
        size_t ref_index = seq::char_index(ref_base);

        // reset all depth counters
        read_depths.assign(read_depths.size(), {});

        // construct values for depth sorting
        dng::pileup::depth_t total_depths;

        // pileup on read counts
        for(std::size_t u = 0; u < data.size(); ++u) {
            for(auto && r : data[u]) {
                if(filter_read(r)) {
                    continue;
                }
                size_t base = seq::base_index(r.aln.seq_at(r.pos));
                total_depths.counts[base] += 1;
                read_depths[u].counts[ base ] += 1;
                // detect overflow of our signed number
                assert(read_depths[u].counts[ base ] >= 0);
                assert(total_depths.counts[ base ] >= 0);
            }
        }
        int dp = 0;
        for(int i=0;i<4;++i) {
            dp += total_depths.counts[i];
        }
        if(dp < min_dp || dp > max_dp) {
            return; // no data at this site
        }
        // check to make sure that we have a positive depths past this point
        assert(dp > 0);
        
        // make sure the ref base will come first
        int first_is_N = 0;
        if(ref_index < 4) {
            total_depths.counts[ref_index] = utility::set_high_bit(total_depths.counts[ref_index]);
        } else {
            first_is_N = 64;
        }
        int color = dng::pileup::raw_counts_to_color(total_depths)+first_is_N;
        assert(0 <= color && color < 128);
        line.location(loc);
        line.resize(color);
        for(int lib = 0; lib < line.num_libraries(); ++lib) {
            for(int n=0; n < line.type_info().width; ++n) {
                line(lib,n) = read_depths[lib].counts[(int)line.type_info().indexes[n]];
            }
        }
        output.Write(line);
    });

    return EXIT_SUCCESS;
}

int process_ad(Pileup::argument_type &arg) {
    using dng::pileup::AlleleDepths;

    if(arg.input.size() != 1) {
        throw std::runtime_error("Argument Error: can only process one ad/tad file at a time.");
    }
    
    io::Ad input{arg.input[0], std::ios_base::in};
    if(!input) {
        throw std::runtime_error("Argument Error: unable to open input file '" + arg.input[0] + "'.");
    }
    if(input.ReadHeader() == 0) {
        throw std::runtime_error("Argument Error: unable to read header from '" + input.path() + "'.");
    }
    
    io::Ad output{arg.output, std::ios_base::out};
    if(!output) {
        throw std::runtime_error("Argument Error: unable to open output file '" + arg.output + "'.");
    }

    output.CopyHeader(input);

    if(arg.body_only && output.format() == io::Ad::Format::AD) {
        throw runtime_error("Argument Error: -B / --body-only not supported with binary output (.ad).");
    }
    if(arg.header_only && output.format() == io::Ad::Format::AD) {
        throw runtime_error("Argument Error: -H / --header-only not supported with binary output (.ad).");
    }
    if(!arg.body_only) {
        output.WriteHeader();
    }
    if(arg.header_only) {
        return EXIT_SUCCESS;
    }

    
    AlleleDepths line;
    line.data().reserve(4*input.libraries().names.size());
    while(input.Read(&line)) {
        output.Write(line);
    }

    return EXIT_SUCCESS;
}

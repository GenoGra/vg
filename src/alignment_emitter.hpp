#ifndef VG_ALIGNMENT_EMITTER_HPP_INCLUDED
#define VG_ALIGNMENT_EMITTER_HPP_INCLUDED

/**
 * \file alignment_emitter.hpp
 *
 * Defines a system for emitting alignments and groups of alignments in multiple formats.
 */

#include <mutex>
#include <thread>
#include <vector>
#include <deque>

#include <htslib/hfile.h>
#include <htslib/hts.h>
#include <htslib/sam.h>

#include <vg/vg.pb.h>
#include <vg/io/protobuf_emitter.hpp>
#include <vg/io/stream_multiplexer.hpp>

namespace vg {
using namespace std;

/**
 * Base class for a sink that takes alignments, possibly with pairing/secondary
 * relationships, and writes them out somewhere.
 *
 * All implementations must be thread safe.
 *
 * All implementations assume OMP threading.
 */
class AlignmentEmitter {
public:
    
    // These batched methods are the ones you need to implement. We batch for
    // efficiency. If there are any locks necessary to ensure thread safety,
    // you must lock once per batch instead of locking for every read.

    /// Emit a batch of Alignments
    virtual void emit_singles(vector<Alignment>&& aln_batch) = 0;
    /// Emit batch of Alignments with secondaries. All secondaries must have is_secondary set already.
    virtual void emit_mapped_singles(vector<vector<Alignment>>&& alns_batch) = 0;
    /// Emit a batch of pairs of Alignments. The tlen_limit_batch, if
    /// specified, is the maximum pairing distance for ewch pair to flag
    /// properly paired, if the output format cares about such things. TODO:
    /// Move to a properly paired annotation that runs with the Alignment.
    virtual void emit_pairs(vector<Alignment>&& aln1_batch, vector<Alignment>&& aln2_batch,
        vector<int64_t>&& tlen_limit_batch) = 0;
    /// Emit the mappings of a batch of pairs of Alignments. All secondaries
    /// must have is_secondary set already. The tlen_limit_batch, if specified,
    /// is the maximum pairing distance for each pair to flag properly paired,
    /// if the output format cares about such things. TODO: Move to a properly
    /// paired annotation that runs with the Alignment.
    ///
    /// Both ends of each pair must have the same number of mappings.
    virtual void emit_mapped_pairs(vector<vector<Alignment>>&& alns1_batch,
        vector<vector<Alignment>>&& alns2_batch, vector<int64_t>&& tlen_limit_batch) = 0;
    
    
    // These single-read methods have default implementations.
    
    /// Emit a single Alignment
    virtual void emit_single(Alignment&& aln);
    /// Emit a single Alignment with secondaries. All secondaries must have is_secondary set already.
    virtual void emit_mapped_single(vector<Alignment>&& alns);
    /// Emit a pair of Alignments. The tlen_limit, if specified, is the maximum
    /// pairing distance to flag properly paired, if the output format cares
    /// about such things. TODO: Move to a properly paired annotation that runs
    /// with the Alignment.
    virtual void emit_pair(Alignment&& aln1, Alignment&& aln2, int64_t tlen_limit = 0);
    /// Emit the mappings of a pair of Alignments. All secondaries must have
    /// is_secondary set already. The tlen_limit, if specified, is the maximum
    /// pairing distance to flag properly paired, if the output format cares
    /// about such things. TODO: Move to a properly paired annotation that runs
    /// with the Alignment.
    ///
    /// Both ends of the pair must have the same number of mappings.
    virtual void emit_mapped_pair(vector<Alignment>&& alns1, vector<Alignment>&& alns2,
        int64_t tlen_limit = 0);
    
    /// Allow destruction through base class pointer.
    virtual ~AlignmentEmitter() = default;
};

/// Get an AlignmentEmitter that can emit to the given file (or "-") in the
/// given format. A table of contig lengths is required for HTSlib formats.
/// Automatically applies per-thread buffering, but needs to know how many OMP
/// threads will be in use.
unique_ptr<AlignmentEmitter> get_alignment_emitter(const string& filename, const string& format, 
    const map<string, int64_t>& path_length,  size_t max_threads);

/**
 * Discards all alignments.
 */
class NullAlignmentEmitter : public AlignmentEmitter {
public:
    inline virtual void emit_singles(vector<Alignment>&& aln_batch) {}
    inline virtual void emit_mapped_singles(vector<vector<Alignment>>&& alns_batch) {}
    inline virtual void emit_pairs(vector<Alignment>&& aln1_batch, vector<Alignment>&& aln2_batch,
        vector<int64_t>&& tlen_limit_batch) {}
    inline virtual void emit_mapped_pairs(vector<vector<Alignment>>&& alns1_batch,
        vector<vector<Alignment>>&& alns2_batch, vector<int64_t>&& tlen_limit_batch) {}
        
};

/**
 * Emit a TSV table describing alignments.
 */
class TSVAlignmentEmitter : public AlignmentEmitter {
public:
    
    /// Create a TSVAlignmentEmitter writing to the given file (or "-")
    TSVAlignmentEmitter(const string& filename, size_t max_threads);

    /// The default destructor should clean up the open file, if any.
    ~TSVAlignmentEmitter() = default;
    
    /// Emit a batch of Alignments.
    virtual void emit_singles(vector<Alignment>&& aln_batch);
    /// Emit a batch of Alignments with secondaries. All secondaries must have
    /// is_secondary set already.
    virtual void emit_mapped_singles(vector<vector<Alignment>>&& alns_batch);
    /// Emit a batch of pairs of Alignments.
    virtual void emit_pairs(vector<Alignment>&& aln1_batch, vector<Alignment>&& aln2_batch,
        vector<int64_t>&& tlen_limit_batch);
    /// Emit the mappings of a batch of pairs of Alignments. All secondaries
    /// must have is_secondary set already.
    virtual void emit_mapped_pairs(vector<vector<Alignment>>&& alns1_batch,
        vector<vector<Alignment>>&& alns2_batch, vector<int64_t>&& tlen_limit_batch);
    
private:

    /// If we are doing output to a file, this will hold the open file. Otherwise (for stdout) it will be empty.
    unique_ptr<ofstream> out_file;
    
    /// This holds a StreamMultiplexer on the output stream, for sharing it
    /// between threads.
    vg::io::StreamMultiplexer multiplexer;

    /// Emit single alignment as TSV.
    /// This is all we use; we don't do anything for pairing.
    void emit(Alignment&& aln_batch);
};

/**
 * Emit Alignments to a stream in SAM/BAM/CRAM format.
 * Thread safe.
 */
class HTSAlignmentEmitter : public AlignmentEmitter {
public:
    /// Create an HTSAlignmentEmitter writing to the given file (or "-") in the
    /// given HTS format ("SAM", "BAM", "CRAM"). path_length must map from
    /// contig name to length to include in the header. Sample names and read
    /// groups for the header will be guessed from the first reads. HTSlib
    /// positions will be read from the alignments' refpos, and the alignments
    /// must be surjected.
    HTSAlignmentEmitter(const string& filename, const string& format, const map<string, int64_t>& path_length, size_t max_threads);
    
    /// Tear down an HTSAlignmentEmitter and destroy HTSlib structures.
    ~HTSAlignmentEmitter();
    
    // Not copyable or movable
    HTSAlignmentEmitter(const HTSAlignmentEmitter& other) = delete;
    HTSAlignmentEmitter& operator=(const HTSAlignmentEmitter& other) = delete;
    HTSAlignmentEmitter(HTSAlignmentEmitter&& other) = delete;
    HTSAlignmentEmitter& operator=(HTSAlignmentEmitter&& other) = delete;
    
    
    /// Emit a batch of Alignments.
    virtual void emit_singles(vector<Alignment>&& aln_batch);
    /// Emit a batch of Alignments with secondaries. All secondaries must have
    /// is_secondary set already.
    virtual void emit_mapped_singles(vector<vector<Alignment>>&& alns_batch);
    /// Emit a batch of pairs of Alignments.
    virtual void emit_pairs(vector<Alignment>&& aln1_batch, vector<Alignment>&& aln2_batch,
        vector<int64_t>&& tlen_limit_batch);
    /// Emit the mappings of a batch of pairs of Alignments. All secondaries
    /// must have is_secondary set already.
    virtual void emit_mapped_pairs(vector<vector<Alignment>>&& alns1_batch,
        vector<vector<Alignment>>&& alns2_batch, vector<int64_t>&& tlen_limit_batch);
    
private:
    
    /// Remember what format we are using.
    string format;
    
    /// Sorte the path length map until the header can be made.
    map<string, int64_t> path_length;
    
    /// We need a samFile that gets opened when we are opened.
    samFile* sam_file;
    /// We need a mutex to synchronize on to protect the output file.
    mutex file_mutex;
    
    /// We need a header
    atomic<bam_hdr_t*> atomic_header;
    /// We also need a header string.
    /// Not atomic, because by the time re wead it we know the header is ready
    /// and nobody is writing to it.
    string sam_header;
    /// If the header isn't present when we want to write, we need a mutex to control creating it.
    mutex header_mutex;
    
    /// Convert an unpaired alignment to HTS format.
    /// Header must have been created already.
    void convert_unpaired(Alignment& aln, vector<bam1_t*>& dest);
    /// Convert a paired alignment to HTS format.
    /// Header must have been created already.
    void convert_paired(Alignment& aln1, Alignment& aln2, int64_t tlen_limit, vector<bam1_t*>& dest);
    
    /// Write and deallocate a bunch of BAM records. Takes care of locking the
    /// file. Header must have been written already.
    void save_records(bam_hdr_t* header, vector<bam1_t*>& records);
    
    /// Make sure that the HTS header has been written.
    /// If it has not been written, blocks until it has been written.
    /// If we end up being the thread to write it, sniff header information from the given alignment.
    /// Returns the header pointer, so we don't have to do another atomic read later.
    bam_hdr_t* ensure_header(const Alignment& sniff);
    
};

/**
 * Emit Alignments to a stream in GAM or JSON format.
 * Thread safe.
 *
 * TODO: Split into Protobuf and JSON versions?
 */
class VGAlignmentEmitter : public AlignmentEmitter {
public:
    /// Create a VGAlignmentEmitter writing to the given file (or "-") in the given
    /// non-HTS format ("JSON", "GAM").
    VGAlignmentEmitter(const string& filename, const string& format, size_t max_threads);
    
    /// Finish and drstroy a VGAlignmentEmitter.
    ~VGAlignmentEmitter();
    
    /// Emit a batch of Alignments.
    virtual void emit_singles(vector<Alignment>&& aln_batch);
    /// Emit a batch of Alignments with secondaries. All secondaries must have
    /// is_secondary set already.
    virtual void emit_mapped_singles(vector<vector<Alignment>>&& alns_batch);
    /// Emit a batch of pairs of Alignments.
    virtual void emit_pairs(vector<Alignment>&& aln1_batch, vector<Alignment>&& aln2_batch,
        vector<int64_t>&& tlen_limit_batch);
    /// Emit the mappings of a batch of pairs of Alignments. All secondaries
    /// must have is_secondary set already.
    virtual void emit_mapped_pairs(vector<vector<Alignment>>&& alns1_batch,
        vector<vector<Alignment>>&& alns2_batch, vector<int64_t>&& tlen_limit_batch);
    
private:

    /// If we are doing output to a file, this will hold the open file. Otherwise (for stdout) it will be empty.
    unique_ptr<ofstream> out_file;
    
    /// This holds a StreamMultiplexer on the output stream, for sharing it
    /// between threads.
    vg::io::StreamMultiplexer multiplexer;
    
    /// We also keep ProtobufEmitters, one per thread, if we are doing protobuf output.
    vector<unique_ptr<vg::io::ProtobufEmitter<Alignment>>> proto;
};

}


#endif

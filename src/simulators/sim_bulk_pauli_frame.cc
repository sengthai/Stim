#include <bit>
#include <cstring>

#include "sim_tableau.h"
#include "sim_bulk_pauli_frame.h"
#include "../simd/simd_util.h"
#include "../probability_util.h"

// Iterates over the X and Z frame components of a pair of qubits, applying a custom BODY to each.
//
// HACK: Templating the body function type makes inlining significantly more likely.
template <typename BODY>
inline void for_each_target_pair(SimBulkPauliFrames &sim, const std::vector<size_t> &targets, BODY body) {
    assert((targets.size() & 1) == 0);
    for (size_t k = 0; k < targets.size(); k += 2) {
        size_t q1 = targets[k];
        size_t q2 = targets[k + 1];
        sim.x_table[q1].for_each_word(sim.z_table[q1], sim.x_table[q2], sim.z_table[q2], body);
    }
}

size_t SimBulkPauliFrames::recorded_bit_address(size_t sample_index, size_t measure_index) const {
    size_t s_high = sample_index >> 8;
    size_t s_low = sample_index & 0xFF;
    size_t m_high = measure_index >> 8;
    size_t m_low = measure_index & 0xFF;
    if (results_block_transposed) {
        std::swap(m_low, s_low);
    }
    return s_low + (m_low << 8) + (s_high << 16) + (m_high * x_table.num_simd_words_minor << 16);
}

SimBulkPauliFrames::SimBulkPauliFrames(size_t init_num_qubits, size_t num_samples, size_t num_measurements, std::mt19937 &rng) :
        num_qubits(init_num_qubits),
        num_samples_raw(num_samples),
        num_measurements_raw(num_measurements),
        x_table(init_num_qubits, num_samples),
        z_table(init_num_qubits, num_samples),
        m_table(num_measurements, num_samples),
        rng_buffer(num_samples),
        rng(rng) {
}

void SimBulkPauliFrames::unpack_sample_measurements_into(size_t sample_index, simd_bits &out) {
    if (!results_block_transposed) {
        do_transpose();
    }
    for (size_t m = 0; m < num_measurements_raw; m += 256) {
        out.u256[m >> 8] = m_table.data.u256[recorded_bit_address(sample_index, m) >> 8];
    }
}

std::vector<simd_bits> SimBulkPauliFrames::unpack_measurements() {
    std::vector<simd_bits> result;
    for (size_t s = 0; s < num_samples_raw; s++) {
        result.emplace_back(num_measurements_raw);
        unpack_sample_measurements_into(s, result.back());
    }
    return result;
}

void SimBulkPauliFrames::unpack_write_measurements(FILE *out, SampleFormat format) {
    if (results_block_transposed != (format == SAMPLE_FORMAT_RAW_UNSTABLE)) {
        do_transpose();
    }

    if (format == SAMPLE_FORMAT_RAW_UNSTABLE) {
        fwrite(m_table.data.u64, 1, m_table.data.num_bits_padded() >> 3, out);
        return;
    }

    simd_bits buf(num_measurements_raw);
    for (size_t s = 0; s < num_samples_raw; s++) {
        unpack_sample_measurements_into(s, buf);

        if (format == SAMPLE_FORMAT_BINLE8) {
            static_assert(std::endian::native == std::endian::little);
            fwrite(buf.u64, 1, (num_measurements_raw + 7) >> 3, out);
        } else if (format == SAMPLE_FORMAT_ASCII) {
            for (size_t k = 0; k < num_measurements_raw; k++) {
                putc_unlocked('0' + buf[k], out);
            }
            putc_unlocked('\n', out);
        } else {
            assert(false);
        }
    }
}

void SimBulkPauliFrames::do_transpose() {
    results_block_transposed = !results_block_transposed;
    blockwise_transpose_256x256(m_table.data.u64, m_table.data.num_bits_padded());
}

void SimBulkPauliFrames::clear() {
    num_recorded_measurements = 0;
    x_table.clear();
    z_table.clear();
    m_table.clear();
    results_block_transposed = false;
}

void SimBulkPauliFrames::clear_and_run(const PauliFrameProgram &program) {
    assert(program.num_measurements == num_measurements_raw);
    clear();
    for (const auto &cycle : program.cycles) {
        for (const auto &op : cycle.step1_unitary) {
            do_named_op(op.name, op.targets);
        }
        for (const auto &collapse : cycle.step2_collapse) {
            RANDOM_KICKBACK(collapse.destabilizer);
        }
        measure_deterministic(cycle.step3_measure);
        reset(cycle.step4_reset);
    }
}

void SimBulkPauliFrames::do_named_op(const std::string &name, const std::vector<size_t> &targets) {
    SIM_BULK_PAULI_FRAMES_GATE_DATA.at(name)(*this, targets);
}

void SimBulkPauliFrames::measure_deterministic(const std::vector<PauliFrameProgramMeasurement> &measurements) {
    for (auto e : measurements) {
        auto q = e.target_qubit;
        auto x = x_table[q].ptr_simd;
        auto x_end = x + x_table[q].num_simd_words;
        auto m = m_table.data.u256 + (recorded_bit_address(0, num_recorded_measurements) >> 8);
        auto m_stride = recorded_bit_address(256, 0) >> 8;
        if (e.invert) {
            while (x != x_end) {
                *m = *x ^ _mm256_set1_epi8(-1);
                x++;
                m += m_stride;
            }
        } else {
            while (x != x_end) {
                *m = *x;
                x++;
                m += m_stride;
            }
        }
        num_recorded_measurements++;
    }
}

void SimBulkPauliFrames::MUL_INTO_FRAME(const SparsePauliString &pauli_string, const simd_bits_range_ref mask) {
    for (const auto &w : pauli_string.indexed_words) {
        for (size_t k2 = 0; k2 < 64; k2++) {
            if ((w.wx >> k2) & 1) {
                x_table[w.index64 * 64 + k2] ^= mask;
            }
            if ((w.wz >> k2) & 1) {
                z_table[w.index64 * 64 + k2] ^= mask;
            }
        }

    }
}

void SimBulkPauliFrames::RANDOM_KICKBACK(const SparsePauliString &pauli_string) {
    rng_buffer.randomize(rng_buffer.num_bits_padded(), rng);
    MUL_INTO_FRAME(pauli_string, rng_buffer);
}

void SimBulkPauliFrames::reset(const std::vector<size_t> &qubits) {
    for (auto q : qubits) {
        x_table[q].clear();
        z_table[q].clear();
    }
}

PauliStringVal SimBulkPauliFrames::get_frame(size_t sample_index) const {
    assert(sample_index < num_samples_raw);
    PauliStringVal result(num_qubits);
    for (size_t q = 0; q < num_qubits; q++) {
        result.x_data[q] = x_table[q][sample_index];
        result.z_data[q] = z_table[q][sample_index];
    }
    return result;
}

void SimBulkPauliFrames::set_frame(size_t sample_index, const PauliStringRef &new_frame) {
    assert(sample_index < num_samples_raw);
    assert(new_frame.num_qubits == num_qubits);
    for (size_t q = 0; q < num_qubits; q++) {
        x_table[q][sample_index] = new_frame.x_ref[q];
        z_table[q][sample_index] = new_frame.z_ref[q];
    }
}

void SimBulkPauliFrames::H_XZ(const std::vector<size_t> &targets) {
    for (auto q : targets) {
        x_table[q].swap_with(z_table[q]);
    }
}

void SimBulkPauliFrames::H_XY(const std::vector<size_t> &targets) {
    for (auto q : targets) {
        z_table[q] ^= x_table[q];
    }
}

void SimBulkPauliFrames::H_YZ(const std::vector<size_t> &targets) {
    for (auto q : targets) {
        x_table[q] ^= z_table[q];
    }
}

void SimBulkPauliFrames::CX(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        z1 ^= z2;
        x2 ^= x1;
    });
}

void SimBulkPauliFrames::CY(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        z1 ^= x2 ^ z2;
        z2 ^= x1;
        x2 ^= x1;
    });
}

void SimBulkPauliFrames::CZ(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        z1 ^= x2;
        z2 ^= x1;
    });
}

void SimBulkPauliFrames::SWAP(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        std::swap(z1, z2);
        std::swap(x1, x2);
    });
}

void SimBulkPauliFrames::ISWAP(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        auto dx = x1 ^ x2;
        auto t1 = z1 ^ dx;
        auto t2 = z2 ^ dx;
        z1 = t2;
        z2 = t1;
        std::swap(x1, x2);
    });
}

void SimBulkPauliFrames::XCX(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        x1 ^= z2;
        x2 ^= z1;
    });
}

void SimBulkPauliFrames::XCY(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        x1 ^= x2 ^ z2;
        x2 ^= z1;
        z2 ^= z1;
    });
}

void SimBulkPauliFrames::XCZ(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        z2 ^= z1;
        x1 ^= x2;
    });
}

void SimBulkPauliFrames::YCX(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        x2 ^= x1 ^ z1;
        x1 ^= z2;
        z1 ^= z2;
    });
}

void SimBulkPauliFrames::YCY(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        auto y1 = x1 ^z1;
        auto y2 = x2 ^z2;
        x1 ^= y2;
        z1 ^= y2;
        x2 ^= y1;
        z2 ^= y1;
    });
}

void SimBulkPauliFrames::YCZ(const std::vector<size_t> &targets) {
    for_each_target_pair(*this, targets, [](auto &x1, auto &z1, auto &x2, auto &z2) {
        z2 ^= x1 ^ z1;
        z1 ^= x2;
        x1 ^= x2;
    });
}

void SimBulkPauliFrames::DEPOLARIZE(const std::vector<size_t> &targets, float probability) {
    RareErrorIterator skipper(probability);
    auto n = targets.size() * num_samples_raw;
    while (true) {
        size_t s = skipper.next(rng);
        if (s >= n) {
            break;
        }
        auto p = 1 + (rng() % 3);
        auto target_index = s / num_samples_raw;
        auto sample_index = s % num_samples_raw;
        auto t = targets[target_index];
        x_table[t][sample_index] ^= p & 1;
        z_table[t][sample_index] ^= p & 2;
    }
}

void SimBulkPauliFrames::DEPOLARIZE2(const std::vector<size_t> &targets, float probability) {
    assert(!(targets.size() & 1));
    RareErrorIterator skipper(probability);
    auto n = (targets.size() * num_samples_raw) >> 1;
    while (true) {
        size_t s = skipper.next(rng);
        if (s >= n) {
            break;
        }
        auto p = 1 + (rng() % 15);
        auto target_index = (s / num_samples_raw) << 1;
        auto sample_index = s % num_samples_raw;
        size_t t1 = targets[target_index];
        size_t t2 = targets[target_index + 1];
        x_table[t1][sample_index] ^= p & 1;
        z_table[t1][sample_index] ^= p & 2;
        x_table[t2][sample_index] ^= p & 4;
        z_table[t2][sample_index] ^= p & 8;
    }
}
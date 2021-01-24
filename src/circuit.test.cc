#include "circuit.h"

#include <gtest/gtest.h>

TEST(circuit, operation_from_line) {
    auto f = [](const std::string line) { return Operation::from_line(line, 0, line.size()); };
    ASSERT_EQ(f("# not an operation"), (Operation{"", OperationData({}, {}, 0)}));

    ASSERT_EQ(f("H 0"), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(f("h 0"), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(f("H 0     "), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(f("     H 0     "), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(f("\tH 0\t\t"), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(f("H 0  # comment"), (Operation{"H", OperationData({0})}));
    ASSERT_EQ(
        f("DEPOLARIZE1(0.125) 4 5  # comment"),
        (Operation{"DEPOLARIZE1", OperationData({4, 5}, {false, false}, 0.125)}));

    ASSERT_EQ(f("  \t Cnot 5 6  # comment   "), (Operation{"CNOT", {{5, 6}}}));

    ASSERT_THROW({ f("H a"); }, std::runtime_error);
    ASSERT_THROW({ f("H 9999999999999999999999999999999999999999999"); }, std::runtime_error);
    ASSERT_THROW({ f("H -1"); }, std::runtime_error);
    ASSERT_THROW({ f("CNOT 0 a"); }, std::runtime_error);
    ASSERT_THROW({ f("CNOT 0 99999999999999999999999999999999"); }, std::runtime_error);
    ASSERT_THROW({ f("CNOT 0 -1"); }, std::runtime_error);
}

TEST(circuit, from_text) {
    ASSERT_EQ(Circuit::from_text(""), Circuit({}));
    ASSERT_EQ(Circuit::from_text("# Comment\n\n\n# More"), Circuit({}));
    ASSERT_EQ(Circuit::from_text("H 0"), Circuit(std::vector<Operation>{{"H", OperationData({0})}}));
    ASSERT_EQ(Circuit::from_text("H 0 \n H 1"), Circuit({{"H", OperationData({0, 1})}}));
    ASSERT_EQ(Circuit::from_text("H 1"), Circuit({{"H", OperationData({1})}}));
    ASSERT_EQ(
        Circuit::from_text("# EPR\n"
                           "H 0\n"
                           "CNOT 0 1"),
        Circuit(std::vector<Operation>{{"H", OperationData({0})}, {"CNOT", OperationData({0, 1})}}));
    ASSERT_EQ(
        Circuit::from_text("M 0 !0 1 !1"),
        Circuit(std::vector<Operation>{{"M", OperationData({0, 0, 1, 1}, {false, true, false, true}, 0)}}));
    ASSERT_EQ(
        Circuit::from_text("# Measurement fusion\n"
                           "H 0\n"
                           "M 0\n"
                           "M 1\n"
                           "M 2\n"
                           "SWAP 0 1\n"
                           "M 0\n"
                           "M 10\n"),
        Circuit(std::vector<Operation>{
            {"H", OperationData({0})},
            {"M", OperationData({0, 1, 2})},
            {"SWAP", OperationData({0, 1})},
            {"M", OperationData({0, 10})}}));
}

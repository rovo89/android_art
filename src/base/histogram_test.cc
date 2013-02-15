#include "gtest/gtest.h"
#include "histogram-inl.h"
#include <sstream>
using namespace art;

//Simple usage:
//  Histogram *hist = new Histogram("SimplePercentiles");
//  Percentile PerValue
//  hist->AddValue(121);
//  hist->AddValue(132);
//  hist->AddValue(140);
//  hist->AddValue(145);
//  hist->AddValue(155);
//  hist->CreateHistogram();
//  PerValue = hist->PercentileVal(0.50); finds the 50th percentile(median).

TEST(Histtest, MeanTest) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("MeanTest");
  double mean;
  for (size_t Idx = 0; Idx < 90; Idx++) {
    hist->AddValue(static_cast<uint64_t>(50));
  }
  mean = hist->Mean();
  EXPECT_EQ(mean, 50);
  hist->Reset();
  hist->AddValue(9);
  hist->AddValue(17);
  hist->AddValue(28);
  hist->AddValue(28);
  mean = hist->Mean();
  EXPECT_EQ(mean, 20.5);
}
;

TEST(Histtest, VarianceTest) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("VarianceTest");
  double variance;
  hist->AddValue(9);
  hist->AddValue(17);
  hist->AddValue(28);
  hist->AddValue(28);
  hist->CreateHistogram();
  variance = hist->Variance();
  EXPECT_EQ(variance, 64.25);
  delete hist;
}
;

TEST(Histtest, Percentile) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("Percentile");
  double PerValue;

  hist->AddValue(20);
  hist->AddValue(31);
  hist->AddValue(42);
  hist->AddValue(50);
  hist->AddValue(60);
  hist->AddValue(70);
  hist->AddValue(98);
  hist->AddValue(110);
  hist->AddValue(121);
  hist->AddValue(132);
  hist->AddValue(140);
  hist->AddValue(145);
  hist->AddValue(155);

  hist->CreateHistogram();
  PerValue = hist->Percentile(0.50);
  EXPECT_GE(PerValue, 70);
  EXPECT_LE(PerValue, 110);

  delete hist;
}
;

TEST(Histtest, UpdateRange) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("UpdateRange");
  double PerValue;

  hist->AddValue(15);
  hist->AddValue(17);
  hist->AddValue(35);
  hist->AddValue(50);
  hist->AddValue(68);
  hist->AddValue(75);
  hist->AddValue(93);
  hist->AddValue(110);
  hist->AddValue(121);
  hist->AddValue(132);
  hist->AddValue(140);  //Median  value
  hist->AddValue(145);
  hist->AddValue(155);
  hist->AddValue(163);
  hist->AddValue(168);
  hist->AddValue(175);
  hist->AddValue(182);
  hist->AddValue(193);
  hist->AddValue(200);
  hist->AddValue(205);
  hist->AddValue(212);
  hist->CreateHistogram();
  PerValue = hist->Percentile(0.50);

  std::string text;
  std::stringstream stream;
  std::string expected =
      "UpdateRange:\t0.99% C.I. 15.262us-214.475us Avg: 126.380us Max: 212us\n";
  hist->PrintConfidenceIntervals(stream, 0.99);

  EXPECT_EQ(expected, stream.str());
  EXPECT_GE(PerValue, 132);
  EXPECT_LE(PerValue, 145);

  delete hist;
}
;

TEST(Histtest, Reset) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("Reset");
  double PerValue;
  hist->AddValue(0);
  hist->AddValue(189);
  hist->AddValue(389);
  hist->Reset();
  hist->AddValue(15);
  hist->AddValue(17);
  hist->AddValue(35);
  hist->AddValue(50);
  hist->AddValue(68);
  hist->AddValue(75);
  hist->AddValue(93);
  hist->AddValue(110);
  hist->AddValue(121);
  hist->AddValue(132);
  hist->AddValue(140);  //Median  value
  hist->AddValue(145);
  hist->AddValue(155);
  hist->AddValue(163);
  hist->AddValue(168);
  hist->AddValue(175);
  hist->AddValue(182);
  hist->AddValue(193);
  hist->AddValue(200);
  hist->AddValue(205);
  hist->AddValue(212);
  hist->CreateHistogram();
  PerValue = hist->Percentile(0.50);

  std::string text;
  std::stringstream stream;
  std::string expected =
      "Reset:\t0.99% C.I. 15.262us-214.475us Avg: 126.380us Max: 212us\n";
  hist->PrintConfidenceIntervals(stream, 0.99);

  EXPECT_EQ(expected, stream.str());
  EXPECT_GE(PerValue, 132);
  EXPECT_LE(PerValue, 145);

  delete hist;
}
;

TEST(Histtest, MultipleCreateHist) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("MultipleCreateHist");
  double PerValue;
  hist->AddValue(15);
  hist->AddValue(17);
  hist->AddValue(35);
  hist->AddValue(50);
  hist->AddValue(68);
  hist->AddValue(75);
  hist->AddValue(93);
  hist->CreateHistogram();
  hist->AddValue(110);
  hist->AddValue(121);
  hist->AddValue(132);
  hist->AddValue(140);  //Median  value
  hist->AddValue(145);
  hist->AddValue(155);
  hist->AddValue(163);
  hist->AddValue(168);
  hist->CreateHistogram();
  hist->AddValue(175);
  hist->AddValue(182);
  hist->AddValue(193);
  hist->AddValue(200);
  hist->AddValue(205);
  hist->AddValue(212);
  hist->CreateHistogram();
  PerValue = hist->Percentile(0.50);

  std::string text;
  std::stringstream stream;
  std::string expected =
      "MultipleCreateHist:\t0.99% C.I. 15.262us-214.475us Avg: 126.380us Max: 212us\n";
  hist->PrintConfidenceIntervals(stream, 0.99);

  EXPECT_EQ(expected, stream.str());
  EXPECT_GE(PerValue, 132);
  EXPECT_LE(PerValue, 145);

  delete hist;
}
;

TEST(Histtest, SingleValue) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("SingleValue");
  double PerValue_10;
  double PerValue_90;

  hist->AddValue(1);
  hist->CreateHistogram();
  PerValue_10 = hist->Percentile(0.1);
  PerValue_90 = hist->Percentile(0.9);

  EXPECT_GT(PerValue_10, 0);
  EXPECT_LT(PerValue_90, 5);

  delete hist;
}
;

TEST(Histtest, SpikyValues) {

  Histogram<uint64_t> *hist = new Histogram<uint64_t>("SpikyValues");
  double PerValue_005;
  double PerValue_995;

  for (size_t Idx = 0; Idx < 300; Idx++) {
    hist->AddValue(rand() % 200);
  }

  hist->AddValue(10000);
  hist->CreateHistogram();
  PerValue_005 = hist->Percentile(0.005);
  PerValue_995 = hist->Percentile(0.995);

  EXPECT_EQ(1.075, PerValue_005);
  EXPECT_EQ(199747, static_cast<int>(PerValue_995 * 1000));
  delete hist;
}
;




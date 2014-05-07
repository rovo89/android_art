#!/bin/bash

set -e

if [ ! -d ./arm ]; then
  echo "Directory ./arm not found."
  exit 1
fi

mkdir -p arm64
dst=`cd arm64 && pwd`
cd arm/
for f in *; do
  cp $f $dst/`echo $f | sed 's/arm/arm64/g'`
done

sed -i 's,ART_COMPILER_DEX_QUICK_ARM_ARM_LIR_H_,ART_COMPILER_DEX_QUICK_ARM64_ARM64_LIR_H_,g' $dst/arm64_lir.h
sed -i 's,ART_COMPILER_DEX_QUICK_ARM_CODEGEN_ARM_H_,ART_COMPILER_DEX_QUICK_ARM64_CODEGEN_ARM64_H_,g' $dst/codegen_arm64.h
sed -i -e 's,ArmMir2Lir,Arm64Mir2Lir,g' -e 's,arm_lir.h,arm64_lir.h,g' -e 's,codegen_arm.h,codegen_arm64.h,g' $dst/*.h $dst/*.cc

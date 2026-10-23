#!/bin/sh
# usage: run.sh <label> <binary> <pattern> [env VAR=val ...]; writes logs/<label>-<pattern>.txt (stdout+stderr), prints exit code to logs/<label>-<pattern>.rc
S=$(cd "$(dirname "$0")" && pwd)
label=$1; bin=$2; pat=$3
export VK_LAYER_SETTINGS_PATH=$S/${VAL_ENV:-env}
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
export VK_LAYER_ENABLES=${LAYER_ENABLES:-VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT}
unset DISPLAY
timeout 300 "$bin" "$pat" > "$S/logs/$label-$pat.txt" 2>&1
echo $? > "$S/logs/$label-$pat.rc"

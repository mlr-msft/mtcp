#! /bin/bash


cur_lib_path=""

for L in `cat ./lib_names`
do
    cur_lib_path="${cur_lib_path} ${L}"
done

echo $cur_lib_path


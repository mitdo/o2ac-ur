#!/bin/bash

RUN="rosrun o2ac_pose_distribution_updater best_scores"

mesh_files=(
    01-BASE.stl
    02-PANEL.stl
    03-PANEL2.stl
    04_37D-GEARMOTOR-50-70.stl
    05_MBRFA30-2-P6.stl
    07_SBARB6200ZZ_30.stl
    08_KZAF1075NA4WA55GA20AA0.stl
    09_EDCS10.stl
    10_CLBPS10_17_4.stl
    11_MBRAC60-2-10.stl
    12_CLBUS6-9-9.5.stl
    13_MBGA30-2.stl
    14_BGPSL6-9-L30-F8.stl
)

obj_names=(
    base
    panel_motor
    panel_bearing
    motor
    motor_pulley
    bearing
    shaft
    end_cap
    bearing_spacer
    output_pulley
    idler_spacer
    idler_pulley
    idler_pin
)

grasp_names=(
    default_grasp
    default_grasp
    default_grasp
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
    grasp_1
)

type_patterns=(
    11111
    01111
    10111
    00111
    11110
    01110
    10110
    00110
    11000
    01000
    10000
)

for i in $(seq 0 $(( ${#obj_names[@]} - 1 ))) ; do
    for type_pattern in ${type_patterns[@]}; do
	output_file=results/${obj_names[$i]}-${type_pattern}.txt
	if [ ! -s $output_file ]; then
	    echo $output_file
	    $RUN /root/o2ac-ur/catkin_ws/src/o2ac_assembly_database/config/wrs_assembly_2020/meshes/${mesh_files[$i]} \
		 /root/o2ac-ur/catkin_ws/src/o2ac_assembly_database/config/wrs_assembly_2020/object_metadata/${obj_names[$i]}.yaml \
		 ${grasp_names[$i]} $type_pattern 0.00001 0.001 100. 100. 100. 1. 1. 1. > $output_file
	fi
    done
done

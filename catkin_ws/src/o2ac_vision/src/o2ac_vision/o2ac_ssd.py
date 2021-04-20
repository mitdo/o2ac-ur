#!/usr/bin/env python
#-*- encoding:utf-8 -*-
#import rospkg
import os
import sys
import glob, json, time
import argparse
import torch
import torch.nn as nn
import torch.backends.cudnn as cudnn
from torch.autograd import Variable
import numpy as np
import cv2
if torch.cuda.is_available():
    torch.set_default_tensor_type('torch.cuda.FloatTensor')

import rospkg

from o2ac_vision.ssd import build_ssd
from o2ac_vision.data import WRS2020_Detection, WRS_ROOT, AnnotationTransform
from o2ac_vision.data import WRS2020_CLASSES as labels

# ID Mapping.
# Chukyo label -> O2AC label(ID, Name, State{front:0, back:1})
o2ac_label = {
    0:(1,"01-BASE",0),
    1:(3,"03-PLATE2",0),
    2:(2,"02-PLATE",0),
    3:(4,"04_37D-GEARMOTOR-50-70",0),
    4:(11,"11_MBRAC60-2-10",0),
    5:(7,"07_SBARB6200ZZ_30",0),
    6:(13,"13_MBGA30-2",0),
    7:(13,"13_b_MBGA30-2",1),
    8:(5,"05_MBRFA30-2-P6",0),
    9:(14,"14_BGPSL6-9-L30-F7",0),
    10:(8,"08_SSFHRT10-75-M4-FC55-G20",0),
    11:(6,"06_MBT4-400",0),
    12:(9,"09_EDCS10",0),
    13:(9,"09_b_EDCS10",1),
    14:(12,"12_CLBUS6-9-9.5",0),
    15:(10,"10_CLBPS10_17_4",0),
    16:(3,"03_b_PLATE2",1),
    17:(2,"02_b_PLATE",1),
    18:(11,"11_b_MBRAC60-2-10",1),
    19:(7,"07_b_SBARB6200ZZ_30",1),
    20:(12,"12_b_CLBUS6-9-9.5",1)
}


#annotation_root = rospack.get_path("wrs_dataset") + "/Annotations/Far/Image-wise/*.json"
annotation_root = "wrs_dataset/Annotations/Far/Image-wise/*.json"
# アノテーションファイルの取得
annotations = glob.glob(annotation_root)

class ssd_detection():

    def __init__(self):
        rospack = rospkg.RosPack()
        fname_weight = rospack.get_path("wrs_dataset") + "/ssd.pytorch/WRS.pth"
        self.net = build_ssd('test', 300, 22)    # initialize SSD
        self.net.load_weights( fname_weight )

    def object_detection(self, im_in, im_vis=None, threshold = 0.6, overlap_threshold = 0.8):
        """ Object detection by SSD
            
        Args:
          im_in(ndarray 3ch): input_image
          im_vis(ndarray 3ch): image for visualization
          threshold(float): threshold of detection
          overlap_threshold(float): threshold for removing overlapped boxes(iou)
                
        Return:
          results... a list of dict(bbox, class_id, score)
        """

        # Preproc
        x = cv2.resize( im_in, (300, 300)).astype(np.float32 )
        x -= (104.0, 117.0, 123.0)
        x = x.astype(np.float32)
        x = x[:, :, ::-1].copy()
        x = torch.from_numpy(x).permute(2, 0, 1)

        #SSD forward
        xx = Variable(x.unsqueeze(0))     # wrap tensor in Variable
        if torch.cuda.is_available():
            xx = xx.cuda()
        y = self.net(xx)

        detections = y.data
        # scale each detection back up to the image
        scale = torch.Tensor(im_in.shape[1::-1]).repeat(2)
        results = list()
        for i in range(detections.size(1)):
            j = 0
            while detections[0,i,j,0] >= threshold:
                score = detections[0,i,j,0]
                label_name = labels[i-1]
                pt = np.clip((detections[0,i,j,1:]*scale).cpu().numpy(),
                             0, None)
                coords = (pt[0], pt[1]), pt[2]-pt[0]+1, pt[3]-pt[1]+1
                j+=1

                bbox = [ int(coords[0][0]), int(coords[0][1]), int(coords[1]), int(coords[2])]
                result = {"bbox": bbox, 
                          "class": o2ac_label[i-1][0], 
                          "confidence": score, 
                          "name":o2ac_label[i-1][1],
                          "state":o2ac_label[i-1][2]}
                results.append( result )
        
        # remove overlapped boxes considering iou        
        results, results_removed = remove_overlapped_boxes( results, overlap_threshold )

        if im_vis is None:
            return results

        for res in results:
            bbox = res["bbox"]
            bb_color = (0,255,0)
            if res["state"] == 1:
                bb_color = (0,0,255)
            im_vis = cv2.rectangle( im_vis, (bbox[0],  bbox[1]),
                                    (bbox[0]+bbox[2], bbox[1]+bbox[3]),
                                    bb_color, 3 )
            cv2.putText( im_vis, res["name"],
                         (bbox[0], bbox[1]),1, 0.7, (255,255,255), 2, cv2.LINE_AA )
            cv2.putText( im_vis, res["name"],
                         (bbox[0], bbox[1]),1, 0.7, (255,0,0), 1, cv2.LINE_AA )
            
        for res in results_removed:
            bbox = res["bbox"]
            bb_color = (127,127,127)
            im_vis = cv2.rectangle( im_vis, (bbox[0],  bbox[1]),
                                    (bbox[0]+bbox[2], bbox[1]+bbox[3]),
                                    bb_color, 3 )
            cv2.putText( im_vis, res["name"],
                         (bbox[0], bbox[1]),1, 0.7, (255,255,255), 2, cv2.LINE_AA )
            cv2.putText( im_vis, res["name"],
                         (bbox[0], bbox[1]),1, 0.7, (0,0,0), 1, cv2.LINE_AA )



        # cv2.imwrite("ssd_result.png", im_vis)

        return results, im_vis

    
def remove_overlapped_boxes( results, threshold=0.8 ):
    """ remove overlapped bbox considering iou score
    
    Note: Box detected in that of "belt" is not removed
    
    Args:
      results(list): ssd_results
      threshold(float): iou threshold
    
    Return:
      list(cleand_results): ssd_results without overlapped boxes
      list(removed_results): removed ssd_results 
    """

    # compute iou for all pair of boxes
    flag = [True for x in range(len(results))]
    for j in range(len(results)):
        bboxA = results[j]["bbox"]
        confA = results[j]["confidence"]
        for i in range(len(results)):
            # class==6 means "belt"
            if (j==i) or (results[j]["class"]==6) or (results[i]["class"]==6):
                continue

            bboxB = results[i]["bbox"]
            confB = results[i]["confidence"]
            iou = compute_iou( bboxA, bboxB )
            if (threshold < iou) and (confA < confB):
                flag[j] = False
    
    # remove boxes
    cleand_results = []
    removed_results = []
    for f, r in zip(flag, results):
        if f is True:
            cleand_results.append(r)
        else:
            removed_results.append(r)
    
    return cleand_results, removed_results


def compute_iou( bboxA, bboxB ):
    """ compute iou of two bounding boxes
    
    Args:
      bboxA(list): coordinates of box A (i,j,w,h) 
      bboxB(list): coordinates of box B (i,j,w,h) 
    
    Return:
      float: iou score
    """

    ix = max(bboxA[0],bboxB[0])
    iy = max(bboxA[1],bboxB[1])
    mx = min(bboxA[0]+bboxA[2],bboxB[0]+bboxB[2])
    my = min(bboxA[1]+bboxA[3],bboxB[1]+bboxB[3])
    area_inter = max(mx-ix,0)*max(my-iy,0)
    area_A = bboxA[2]*bboxA[3]
    area_B = bboxB[2]*bboxB[3]

    iou = area_inter / (area_A + area_B - area_inter)
    return iou
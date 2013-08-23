;;;; -*- Lisp -*-
;;;;
;;;; Copyright (c) 2013, Georgia Tech Research Corporation
;;;; All rights reserved.
;;;;
;;;; Author(s): Neil T. Dantam <ntd@gatech.edu>
;;;; Georgia Tech Humanoid Robotics Lab
;;;; Under Direction of Prof. Mike Stilman
;;;;
;;;;
;;;; This file is provided under the following "BSD-style" License:
;;;;
;;;;
;;;;   Redistribution and use in source and binary forms, with or
;;;;   without modification, are permitted provided that the following
;;;;   conditions are met:
;;;;
;;;;   * Redistributions of source code must retain the above copyright
;;;;     notice, this list of conditions and the following disclaimer.
;;;;
;;;;   * Redistributions in binary form must reproduce the above
;;;;     copyright notice, this list of conditions and the following
;;;;     disclaimer in the documentation and/or other materials provided
;;;;     with the distribution.
;;;;
;;;;   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
;;;;   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
;;;;   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
;;;;   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
;;;;   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
;;;;   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
;;;;   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
;;;;   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
;;;;   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
;;;;   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
;;;;   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
;;;;   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
;;;;   POSSIBILITY OF SUCH DAMAGE.

;; Author: Neil T. Dantam

(in-package :piranha)



(defparameter *ctrl-channel* nil)
(defparameter *state-channel* nil)

(defvar *last-traj*)


(defun pir-start ()
  (assert (null *ctrl-channel*))
  (setq *ctrl-channel* (ach::open-channel "pir-ctrl"))
  (setq *state-channel* (ach::open-channel "pir-state")))

(defun pir-stop ()
  (ach::close-channel *ctrl-channel*)
  (setq *ctrl-channel* nil ))


(cffi:defcstruct pir-message
  (type :char :count 64)
  (n :uint64))


(cffi:defcstruct pir-cstate
  (q :double :count 29)
  (dq :double :count 29)

  (f-l :double :count 6)
  (f-r :double :count 6)

  (s-l :double :count 8)
  (s-r :double :count 8)

  (j-l :double :count #.(* 7 6))
  (j-r :double :count #.(* 7 6)))


(defstruct pir-state
  q         ; joint angles
  dq        ; joint velocities
  s-l       ; left pose quaternion
  s-r       ; right pose quaternion
  r-l       ; left rotion quaternion
  r-r       ; right rotion quaternion
  x-l       ; left translation vector
  x-r       ; right translation vector
  f-l
  f-r
  )


(defun read-doubles (pointer size)
  (let ((x (make-array size :element-type 'double-float)))
    (dotimes (i size)
      (setf (aref x i)
            (mem-aref pointer :double i)))
    x))


(defun get-state ()
  (with-foreign-object (state 'pir-cstate)
    (ach:get-pointer *state-channel* state (foreign-type-size 'pir-cstate)
                     :wait t :last t)
    (labels ((extract (x n)
               (read-doubles (foreign-slot-pointer state 'pir-cstate x) n)))
      (let ((s-l (extract 's-l 8))
            (s-r (extract 's-r 8)))
        (multiple-value-bind (r-l x-l) (amino::tf-duqu2qv s-l)
          (multiple-value-bind (r-r x-r) (amino::tf-duqu2qv s-r)
            (make-pir-state
             :q (extract 'q 29)
             :dq (extract 'dq 29)
             :f-l (extract 'f-l 6)
             :f-r (extract 'f-r 6)
             :s-l s-l
             :s-r s-r
             :r-l r-l
             :r-r r-r
             :x-l x-l
             :x-r x-r)))))))



(defun pir-set-mode (msg mode)
  (assert (< (length mode) 63))
  (labels ((setit (i val)
             (setf (mem-aref (foreign-slot-pointer msg 'pir-message 'type)
                             :char i)
                   val)))

    (loop for i below (length mode)
       do (setit i (char-code (aref mode i))))
    (setit (length mode) 0)))

(defun pir-message (mode &optional data)
  (with-foreign-pointer (msg (+ (foreign-type-size 'pir-message)
                                (* 8 (length data)))
                             msg-size)
    (pir-set-mode msg mode)
    (setf (foreign-slot-value msg 'pir-message 'n)
          (length data))
    (dotimes (i (length data))
      (let ((pointer (inc-pointer (foreign-slot-pointer msg 'pir-message 'n)
                                  (* 8 (1+ i))))
            (x (elt data i)))
        (etypecase x
          (fixnum
           (setf (mem-aref pointer :int64) x))
          (double-float
           (setf (mem-aref pointer :double) x)))))
    (ach::put-pointer *ctrl-channel* msg msg-size)))

(defstruct trajx-point
  pose
  time)

(defun trajx-point-data (points)
  (let* ((n (length points))
         (data (amino::make-vec (* 9 (length points)))))
    (dotimes (i n)
      (let ((point (elt points i))
            (offset (* i n)))
        (setf (aref data offset)
              (trajx-point-time point))
        (replace data (amino::matrix-data (trajx-point-pose point))
                 :start1 (+ 1 offset) :end1 (+ 9 offset))))
    data))

(defun pir-go (points &key
               (state (get-state)))
  (setq *last-traj* (append (list (make-trajx-point :pose (pir-state-s-l state)
                                                    :time 0d0))
                            points))
  (pir-message "trajx" (trajx-point-data points)))

(defun pir-go-rel (&key
                   (x (amino::col-vector 0 0 0))
                   (time 5d0)
                   (r amino::+tf-quat-ident+))
  (let ((state (get-state)))
    (let* ((S-rel (amino::tf-qv2duqu r x))
           (S-1 (amino::tf-duqu-mul (pir-state-s-l state) S-rel)))
      (pir-go (list (make-trajx-point :pose S-1
                                      :time time))
              :state state))))

(defun pir-screw (x theta &key (time 5d0))
  (pir-go-rel :x (amino::col-vector x 0 0)
              :r (amino::tf-xangle2quat theta)
              :time time))

(defun pir-rotate (r &key (time 10d0))
  (let ((state (get-state)))
    (pir-go (list (make-trajx-point :pose (amino::tf-qv2duqu r (pir-state-x-l state))
                                    :time time))
            :state state)))


(defun pir-set (q &key (time 10d0))
  (check-type q (simple-array double-float (7)))
  (let ((vec (amino::make-vec 8)))
    (setf (aref vec 0) time)
    (replace vec q :start1 1)
    (pir-message "trajq" vec)))

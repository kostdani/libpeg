;;; guix.scm - Guix package declaration for libpeg.
;;;
;;; Build with:  guix build -f guix.scm
;;; Dev shell:   guix shell -f guix.scm        (or: guix shell cmake)
;;; Install:     guix package -f guix.scm

(use-modules (guix packages)
             (guix build-system cmake)
             (guix gexp)
             ((guix licenses) #:prefix license:)
             (srfi srfi-1))

(define %source-dir (dirname (current-filename)))

(package
  (name "libpeg")
  (version "0.1.0")
  (source (local-file %source-dir
                      #:recursive? #t
                      #:select?
                      (lambda (file stat)
                        ;; Skip build directories and their artifacts.
                        (not (any (lambda (dir)
                                    (string-prefix? dir file))
                                  '("build" "build-asan"))))))
  (build-system cmake-build-system)
  (arguments
   (list #:tests? #t))
  (synopsis "Fast incremental PEG parsing in C")
  (description
   "libpeg is a C implementation of the incremental PEG parser described in
@cite{Fast Incremental PEG Parsing} (Yedidia and Chong, SLE 2021).
A grammar compiles to a program for an LPeg-style parsing machine with a
memoization table backed by a lazily-shifted interval tree, so that after
a full parse, reparsing an edited input costs time proportional to the
edit rather than the input.")
  (home-page #f)
  (license license:expat))

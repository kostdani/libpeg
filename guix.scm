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
                        ;; Skip build trees, VCS metadata and editor
                        ;; droppings: what goes into the store is the
                        ;; source.  Guix hands this predicate absolute
                        ;; paths; the prefix tests cover relative ones.
                        (not (any (lambda (dir)
                                    (or (string-prefix? dir file)
                                        (string-suffix?
                                         (string-append "/" dir) file)))
                                  '("build" "build-asan" "build-term"
                                    "build-werror" "cmake-build-debug"
                                    ".git" ".idea"))))))
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
edit rather than the input.  A header-only C++ wrapper over the same ABI
is installed alongside the C headers.")
  (home-page #f)
  (license license:expat))

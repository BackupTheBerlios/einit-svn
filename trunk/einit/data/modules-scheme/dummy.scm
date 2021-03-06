#!

; notice : string -> boolean
; (notice "hello world, from a scheme module")

; critical : string -> boolean
; (critical "hello world, from a scheme module, as a critical message")


; make-module : symbol string list(symbol string*)* -> boolean
(if (make-module 's-hello "some dummy module"
                 (list 'provides "s-dummy")
;                 (list 'requires "c" "d")
;                 (list 'after yes"e" "f")
;                 (list 'before "g" "h")
    )
    (notice "dummy module created")
    (critical "couldn't create dummy module"))

; define-module-action : symbol symbol procedure -> boolean
(define-module-action 's-hello 'enable
 (lambda (status)
  (begin
   (feedback status "abc")
   (feedback status "def")
   (feedback status "ghi")
   (shell "echo hello world; sleep 10; true"
          'feedback: status))))

(define-module-action 's-hello 'disable
 (lambda (status) #t))

; make-event : symbol ['symbol: value]* [string] [list(string)] -> einit-event
;(display
; (make-event 'core/update-configuration
;  'status: 22
;  "some string"
;  '("a" "b" "c")))

; event-emit : einit-event -> #void
;(event-emit (make-event 'core/update-configuration))

; event-listen : symbol procedure(event -> #void) -> boolean
;(event-listen 'any
; (lambda (event)
;  (begin
;   (display "dummy.scm: received event: ")
;   (display event)
;  (newline))))

; set-configuration! : symbol list(cons(string . string)) -> #void
;(set-configuration! 's-config-meow
; '(("s" . "hello") ("i" . "5") ("b" . "false") ("id" . "meow")))

;(display
; (get-configuration 's-config-meow))

; get-configuration : symbol -> list(cons(string . string))
;(display
; (get-configuration 'subsystem-scheme-modules))

!#

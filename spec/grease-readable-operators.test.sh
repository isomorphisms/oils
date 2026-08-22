## suite: osh
## oils_failures_allowed: 0
## compare_shells:

#### Grease white square brackets, conjunction, and condition negation
x=hello
⟦ -n "$x" ∧ ¬ "$x" = no ⟧ ∧ echo yes
## stdout: yes

#### Grease disjunction in command context
false ∨ echo fallback
## stdout: fallback

#### Grease negation in command context
¬ false ∧ echo negated
## stdout: negated

#### Grease disjunction inside a condition
⟦ '' ∨ value ⟧ ∧ echo true
## stdout: true

#### Grease operator glyphs stay literal when quoted
printf '%s\n' '⟦ ∧ ∨ ¬ ⟧' "⟦ ∧ ∨ ¬ ⟧"
## STDOUT:
⟦ ∧ ∨ ¬ ⟧
⟦ ∧ ∨ ¬ ⟧
## END

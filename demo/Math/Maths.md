---
tags: [demo, maths]
---

# Maths

What to look for: inline maths should sit on the text baseline without
breaking line height, display maths should center on its own block, and the
malformed formula at the end should fail visibly (a red error box or the
raw LaTeX shown) rather than silently vanishing or crashing the note.

## Inline

Euler's identity, inline: $e^{i\pi} + 1 = 0$. The quadratic formula, inline:
$x = \frac{-b \pm \sqrt{b^2-4ac}}{2a}$. A sentence can hold several: mass-energy
$E=mc^2$ next to the golden ratio $\varphi = \frac{1+\sqrt{5}}{2}$.

## Display

$$
\int_{0}^{\infty} e^{-x^2}\,dx = \frac{\sqrt{\pi}}{2}
$$

## Fractions and sums

$$
\sum_{n=1}^{\infty} \frac{1}{n^2} = \frac{\pi^2}{6}
\qquad
\prod_{k=1}^{n} k = n!
$$

## A matrix

$$
\begin{pmatrix}
a & b & c \\
d & e & f \\
g & h & i
\end{pmatrix}
\begin{pmatrix} x \\ y \\ z \end{pmatrix}
=
\begin{pmatrix} ax+by+cz \\ dx+ey+fz \\ gx+hy+iz \end{pmatrix}
$$

## Aligned equations

$$
\begin{aligned}
(a+b)^2 &= a^2 + 2ab + b^2 \\
(a-b)^2 &= a^2 - 2ab + b^2 \\
a^2 - b^2 &= (a+b)(a-b)
\end{aligned}
$$

## A malformed formula

Left unclosed on purpose, to check the failure mode:

$$
\frac{1}{2
$$

And an inline one with an unknown command:

Inline malformed: $\notarealcommand{x}$.

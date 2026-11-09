#include "core/worker.hpp"

#include <cstdio>

#include "core/loop.hpp"

int main() {
    glint::Loop loop;
    glint::Workers workers(loop, 1);
    int iterations = 0, first_iteration = 0, steps = 0, value = 0, failures = 0;
    loop.set_before_poll([&] { ++iterations; });
    workers.post([&] { value = 42; },
                 [&] {
                     if (value != 42) ++failures;
                     first_iteration = iterations;
                     ++steps;
                     workers.post_main([&] {
                         if (iterations <= first_iteration) ++failures;
                         first_iteration = iterations;
                         ++steps;
                     });
                     workers.post_main([&] {
                         // Both continuations were already queued. Rendering must still get a turn
                         // between them, rather than the second running in the first one's
                         // dispatch.
                         if (iterations <= first_iteration || steps != 2) ++failures;
                         ++steps;
                         loop.quit();
                     });
                 });
    if (!loop.run()) ++failures;
    if (steps != 3) ++failures;
    std::printf("worker continuations: %d steps, %d failures\n", steps, failures);
    return failures != 0;
}

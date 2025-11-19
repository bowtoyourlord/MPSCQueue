// This is just a demo file

#include <iostream>
#include <thread>
#include <atomic>
#include "MPSCQueue.h"

MPSCQueue<int> mpscqueue;
static std::atomic<int> activeProducers{0};


void producerDEMO(int start)
{
    activeProducers.fetch_add(1);
    static const int dataAnalysedByProducer = 10'000'000;

    for (int i = start; i < start + dataAnalysedByProducer; ++i) {
        // usually MPSC is used when there is high push load, but some things are filtered out and should not be processed, because if everything is processed, it is algorithmically not possible to guarantee that 1 thread (consumer) could be capable of handling > 1 thread, hence arbitrary %5 as filter emulation, aka 1 in 5 packets pass
        if (i%5 == 0) {
            if (!mpscqueue.push(i)) {
                // Rare case: buffer is full
                static std::atomic<int> errors = 0;
                if (errors.fetch_add(1) < 10) {
                    std::cout << "[producer] push failed for " << i << "\n";
                } else {
                    std::cout << "Too much fails, break" << "\n";
                    break;
                }
            }
        }
    }
    activeProducers.fetch_sub(1);
}

void consumerDEMO()
{
    int processed = 0;
    int next_report = 500'000; // using '%' is very expensive, simple comparsion is way cheaper
    while (true) {
        auto val = mpscqueue.pop();
        if (val.has_value()) {
            ++processed;
            if (processed == next_report) {
                next_report += 500'000;
                std::cout << "[consumer] processed: " << processed << "\n";
            }
        } else {
            if (!activeProducers.load()) break;
        }
    }

    std::cout << "[consumer] done, total processed = " << processed << "\n";
}

int main()
{
    std::cout << "Observe the speed.\n";

    std::thread p1(producerDEMO, 0);
    std::thread p2(producerDEMO, -1'000'000'000);
//    std::thread p3(producerDEMO, 1'000'000'000);
//    std::thread p4(producerDEMO, 50'000'000);
//    std::thread p5(producerDEMO, -50'000'000);

    std::thread c(consumerDEMO);
    
    p1.join();
    p2.join();
//    p3.join();
//    p4.join();
//    p5.join();

    c.join();

    std::cout << "Showcase completed.\n";
    return EXIT_SUCCESS;
}

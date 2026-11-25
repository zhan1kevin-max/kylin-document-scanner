#include <QApplication>
#include <QPushButton>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QPushButton button;
    bool clicked = false;
    const auto buttonConnection = QObject::connect(&button, &QPushButton::clicked,
                                                   &application, [&] { clicked = true; });
    if (!buttonConnection) {
        std::cerr << "Qt typed button signal could not be connected. Check PIC flags.\n";
        return 1;
    }
    button.click();
    if (!clicked) {
        std::cerr << "Qt button click did not reach its connected handler.\n";
        return 1;
    }

    QTimer timer;
    timer.setSingleShot(true);
    bool timedOut = false;
    const auto timerConnection = QObject::connect(&timer, &QTimer::timeout,
                                                  &application, [&] {
        timedOut = true;
        application.quit();
    });
    if (!timerConnection) {
        std::cerr << "Qt typed timer signal could not be connected. Check PIC flags.\n";
        return 1;
    }
    QTimer::singleShot(2000, &application, [&] { application.exit(2); });
    timer.start(1);
    const int status = application.exec();
    if (status != 0 || !timedOut) {
        std::cerr << "Qt timer did not deliver its timeout signal.\n";
        return 1;
    }
    std::cout << "Qt button and timer typed-signal regression tests passed.\n";
    return 0;
}

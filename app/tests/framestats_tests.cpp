#include "framestats.h"
#include "framemonitor.h"

#include <QExposeEvent>
#include <QQuickWindow>
#include <QScreen>
#include <QtTest>
#include <limits>

class FrameStatsTests : public QObject {
    Q_OBJECT
private slots:
    void rates_data() {
        QTest::addColumn<double>("rate");
        QTest::newRow("60Hz") << 60.0;
        QTest::newRow("fractional-59.94Hz") << 59.94;
        QTest::newRow("120Hz") << 120.0;
        QTest::newRow("144Hz") << 144.0;
        QTest::newRow("240Hz") << 240.0;
    }
    void rates() {
        QFETCH(double, rate);
        qint64 now = 0;
        FrameStats stats([&] { return now; });
        stats.setRefreshRate(rate);
        const qint64 period = qRound64(1e9 / rate);
        stats.frameSwapped();
        for (double periods : {1.0, 1.49, 1.51, 2.0}) {
            now += qRound64(period * periods);
            stats.syncBegan(); stats.renderBegan(); stats.renderEnded(); stats.frameSwapped();
        }
        const auto summary = stats.summary();
        QCOMPARE(summary.frames, 4);
        QCOMPARE(summary.missed, 2);
        QCOMPARE(summary.slowest.size(), size_t(2));
        QVERIFY(qAbs(summary.targetHz - rate) < 1e-6);
        QVERIFY(!summary.mixedRefreshRates);
        QVERIFY(qAbs(summary.intervalMax - period * 2 / 1e6) < 0.001);
    }
    void changingRateKeepsEachSamplesOwnBudgetAndDropsTheSwitchGap() {
        qint64 now = 0;
        FrameStats stats([&] { return now; });
        stats.setRefreshRate(60);
        stats.frameSwapped();
        now += 16'666'667; stats.frameSwapped();
        stats.setRefreshRate(120);
        now += 5'000'000'000; stats.frameSwapped();
        now += 8'333'333; stats.frameSwapped();
        auto summary = stats.summary();
        QCOMPARE(summary.frames, 2);
        QCOMPARE(summary.missed, 0);
        QVERIFY(summary.mixedRefreshRates);
        QCOMPARE(summary.targetHz, 120.0);
        // The previous 60 Hz frame must not be reclassified against 120 Hz.
        QCOMPARE(summary.intervalMax, 16.666667);
        stats.reset();
        QCOMPARE(stats.summary().frames, 0);
        QCOMPARE(stats.summary().targetHz, 120.0);
    }
    void invalidRatesHaveAFiniteSixtyHzFallback() {
        FrameStats stats;
        for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                            std::numeric_limits<double>::quiet_NaN(), 5000.0}) {
            stats.setRefreshRate(144);
            stats.setRefreshRate(rate);
            QCOMPARE(stats.summary().targetHz, 60.0);
        }
    }
    void exposurePauseIsNotALateFrame() {
        qint64 now = 0;
        FrameStats stats([&] { return now; });
        QQuickWindow window;
        new FrameMonitor(&window, &stats);
        stats.frameSwapped();
        now += 16'666'667; stats.frameSwapped();
        const int recorded = stats.summary().frames;
        QExposeEvent event{QRegion()};
        QCoreApplication::sendEvent(&window, &event);
        now += 60'000'000'000; stats.frameSwapped();
        now += 16'666'667; stats.frameSwapped();
        QCOMPARE(stats.summary().frames, recorded + 1);
        QCOMPARE(stats.summary().missed, 0);
    }
    void monitorFollowsScreenRateChanges() {
        FrameStats stats;
        QQuickWindow window;
        new FrameMonitor(&window, &stats);
        QVERIFY(window.screen());
        QCOMPARE(stats.summary().targetHz, double(window.screen()->refreshRate()));
        QVERIFY(QMetaObject::invokeMethod(window.screen(), "refreshRateChanged", Qt::DirectConnection,
                                         Q_ARG(qreal, 144.0)));
        QCOMPARE(stats.summary().targetHz, 144.0);
    }
};

QTEST_MAIN(FrameStatsTests)
#include "framestats_tests.moc"

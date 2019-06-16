#include <QtCore>
#include <QProcess>
#include <cstdlib>
#include "danmakulauncher.h"
#include "mkv_header.h"
#include "dmc_py.h"

#define __PICK(n, i)   (((n)>>(8*i))&0xFF)

DanmakuLauncher::DanmakuLauncher(QStringList args)
{
    fifo = QString("/tmp/qlp-%1").arg(QUuid::createUuid().toString());
    url = args.at(0);
    record_file = args.at(1);
    stream_url = getStreamUrl(url);
    if (stream_url == "null") {
        exit(1);
        return;
    }
    launch_timer = new QTimer(this);
    launch_timer->setInterval(200);
    launch_timer->start();

    QProcess::execute(QString("mkfifo %1").arg(fifo));
    fifo_file = new QFile(fifo);
    initPlayer();
    if (!fifo_file->open(QIODevice::WriteOnly)) {
        qCritical() << "Cannot open fifo!";
        exit(1);
    }
    fifo_file->write(reinterpret_cast<const char*>(mkv_header), mkv_header_len);
    fifo_file->flush();

    for (int i = 0; i < 30; i++) {
        danmaku_channel[i].length = 0;
        danmaku_channel[i].duration = 0;
        danmaku_channel[i].begin_pts = 0;
    }

    initDmcPy();
    live_checker = new QProcess(this);
    connect(launch_timer, &QTimer::timeout, this, &DanmakuLauncher::launchDanmaku);
    connect(live_checker, &QProcess::readyRead, this, &DanmakuLauncher::getLiveStatus);

}

DanmakuLauncher::~DanmakuLauncher()
{
    qCritical() << "Bye!";
    launch_timer->stop();
    launch_timer->deleteLater();
    fifo_file->close();
    fifo_file->remove();
    fifo_file->deleteLater();
    live_checker->disconnect();
    player->disconnect();
    dmcPyProcess->disconnect();
    live_checker->terminate();
    live_checker->waitForFinished(3000);
    live_checker->deleteLater();
    dmcPyProcess->terminate();
    dmcPyProcess->waitForFinished(3000);
    dmcPyProcess->deleteLater();
    player->terminate();
    player->waitForFinished(3000);
    player->deleteLater();
}

void DanmakuLauncher::initDmcPy()
{
    timer.start();
    QStringList dmcPy;
    dmcPy.append("-c");
//    dmcPy.append("exec(\"\"\"\\nimport time, sys\\nfrom danmu import DanMuClient\\ndef pp(msg):\\n    print(msg.encode(sys.stdin.encoding, 'ignore').\\n        decode(sys.stdin.encoding))\\n    sys.stdout.flush()\\ndmc = DanMuClient(sys.argv[1])\\nif not dmc.isValid(): \\n    print('Unsupported danmu server')\\n    sys.exit()\\n@dmc.danmu\\ndef danmu_fn(msg):\\n    pp('[%s] %s' % (msg['NickName'], msg['Content']))\\ndmc.start(blockThread = True)\\n\"\"\")");
    dmcPy.append("exec(\"\"\"\\nimport asyncio, sys\\nimport danmaku\\n\\ndef cb(m):\\n    if m['msg_type'] == 'danmaku':\\n        print(f'[{m[\"name\"]}] {m[\"content\"]}'.encode(sys.stdin.encoding, 'ignore').\\n              decode(sys.stdin.encoding))\\n        sys.stdout.flush()\\n\\n\\nasync def main():\\n    dmc = danmaku.DanmakuClient(sys.argv[1], cb)\\n    await dmc.start()\\n\\nasyncio.run(main())\\n\"\"\")");
    dmcPy.append(url);
    dmcPyProcess = new QProcess(this);
    connect(dmcPyProcess, &QProcess::readyRead, this, &DanmakuLauncher::loadDanmaku);
    dmcPyProcess->start("python3", dmcPy);
    qDebug() << "Danmaku process initialized!";
}

void DanmakuLauncher::initPlayer()
{
    QStringList player_args;

    player_args.append("-c");
    player_args.append(getPlayerCMD(url));
    player = new QProcess(this);
    player->setReadChannel(QProcess::StandardError);
    connect(player, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [=](int exitCode, QProcess::ExitStatus exitStatus){
        Q_UNUSED(exitStatus);
        QCoreApplication::exit(exitCode);
    });
    connect(player, &QProcess::readyReadStandardError, this, &DanmakuLauncher::printPlayerProcess);
    player->start("sh", player_args);
    qDebug() << "Player process initialized!";
}

QString DanmakuLauncher::getPlayerCMD(QString url)
{
    QString ret;
    QString extra_args;
//    QString headers("user-agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/79.0.3945.88 Safari/537.36");
    extra_args = record_file.compare("null") != 0 ? QString("--stream-record '%1'").arg(record_file) : " ";
    if (url.contains("huya.com")) {
        ret = QString("ffmpeg -headers 'User-Agent: Wget/1.20.3 (linux-gnu)' -i '%1' -c copy -f flv - | "
                      "ffmpeg -i - -i '%2' -map 0:v -map 0:a -map 1:0 -c copy -f matroska -metadata title='%4' - 2>/dev/null| "
                      "mpv %3 --vf 'lavfi=\"fps=fps=60:round=down\"' - 1>/dev/null 2>&1").arg(stream_url).arg(fifo).arg(extra_args).arg(title);
    } else {
        ret = QString("ffmpeg -nocopyts -headers 'User-Agent: Wget/1.20.3 (linux-gnu)' -i '%1' -i '%2' -map 0:v -map 0:a -map 1:0 -c copy -f matroska -metadata title='%4' - | "
                      "mpv %3 --vf 'lavfi=\"fps=fps=60:round=down\"' - 1>/dev/null 2>&1").arg(stream_url).arg(fifo).arg(extra_args).arg(title);
    }
    return ret;
}

int DanmakuLauncher::getDankamuDisplayLength(QString dm, int fontsize)
{
    int ascii_num = 0;
    int dm_size = dm.size();
    const QChar *data = dm.constData();
    while (dm_size > 0) {
        if (data->unicode() < 128) {
            ++ascii_num;
        }
        ++data;
        --dm_size;
    }
    return (fontsize * 0.75 * dm.size()) - (fontsize * 0.25 * ascii_num);
}


void DanmakuLauncher::loadDanmaku()
{
    while(dmcPyProcess->canReadLine()) {
        danmaku_queue.enqueue(dmcPyProcess->readLine());
    }
}

void DanmakuLauncher::launchDanmaku()
{
    if (check_counter % 25 == 24) {
        launchLiveChecker();
        check_counter = 0;
    } else {
        ++check_counter;
    }
    if (danmaku_queue.isEmpty()) {
        launchVoidDanmaku();
        return;
    }
    int display_length = 0;
    double speed = 0.0;
    int avail_dc = -1;
    qint64 c_pts = timer.elapsed() + pts;
    QString dm;
    QString speaker;
    QString ass_event;
    QByteArray bin_out;
    quint64 buf;
    while (!danmaku_queue.isEmpty()) {
        dm = danmaku_queue.dequeue();
        dm.chop(1);
        qInfo().noquote() << dm;
        dm.remove(0, 1);
        speaker = dm.section(QChar(']'), 0, 0);
        dm = dm.section(QChar(']'), 1, -1);
        dm.remove(0, 1);
        display_length = getDankamuDisplayLength(dm, font_size);
        speed = 60.0 * 4.0 * sqrt(sqrt(display_length/250.0)) + 0.5;
        avail_dc = getAvailDanmakuChannel(speed);
        if (avail_dc >= 0) {
            danmaku_channel[avail_dc].length = display_length;
            danmaku_channel[avail_dc].duration = 1000.0 * (1920.0 + display_length) / speed;
            danmaku_channel[avail_dc].begin_pts = c_pts;
            QByteArray tmp;
            ass_event = QString("%4,0,Default,%5,0,0,0,,{\\move(1920,%1,%2,%1)}%3").arg(QString().number(avail_dc*(font_size))).arg(QString().number(0-display_length)).arg(dm).arg(QString().number(read_order)).arg(speaker);
            ++read_order;
            tmp = ass_event.toLocal8Bit();
            tmp.prepend((char)0x00);
            tmp.prepend((char)0x00);
            tmp.prepend((char)0x00);
            tmp.prepend((char)0x81);
            buf = tmp.size() | 0x10000000;
            tmp.prepend(__PICK(buf, 0));
            tmp.prepend(__PICK(buf, 1));
            tmp.prepend(__PICK(buf, 2));
            tmp.prepend(__PICK(buf, 3));
            tmp.prepend(0xa1);
            buf = 0x849b;
            tmp.append((const char *)&buf, 2);
            buf = danmaku_channel[avail_dc].duration;
            tmp.append(__PICK(buf, 3));
            tmp.append(__PICK(buf, 2));
            tmp.append(__PICK(buf, 1));
            tmp.append(__PICK(buf, 0));
            buf = tmp.size() | 0x10000000;
            tmp.prepend(__PICK(buf, 0));
            tmp.prepend(__PICK(buf, 1));
            tmp.prepend(__PICK(buf, 2));
            tmp.prepend(__PICK(buf, 3));
            tmp.prepend(0xa0);
            bin_out.append(tmp);
        } else {
            return;
        }
    }
    buf = (pts + timer.elapsed());
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    bin_out.prepend(__PICK(buf, 4));
    bin_out.prepend(__PICK(buf, 5));
    bin_out.prepend(__PICK(buf, 6));
    bin_out.prepend(__PICK(buf, 7));
    bin_out.prepend(0x88);
    bin_out.prepend(0xe7);
    buf = bin_out.size() | 0x0100000000000000;
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    bin_out.prepend(__PICK(buf, 4));
    bin_out.prepend(__PICK(buf, 5));
    bin_out.prepend(__PICK(buf, 6));
    bin_out.prepend(__PICK(buf, 7));
    buf = 0x1f43b675;
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    fifo_file->write(bin_out);
    fifo_file->flush();
}

void DanmakuLauncher::launchVoidDanmaku()
{
    QString ass_event;
    QByteArray bin_out;
    quint64 buf;
    QByteArray tmp;
    ass_event = QString("%1,0,Default,QLivePlayer-Empty,20,20,2,,").arg(QString().number(read_order));
    ++read_order;
    tmp = ass_event.toLocal8Bit();
    tmp.prepend((char)0x00);
    tmp.prepend((char)0x00);
    tmp.prepend((char)0x00);
    tmp.prepend((char)0x81);
    buf = tmp.size() | 0x10000000;
    tmp.prepend(__PICK(buf, 0));
    tmp.prepend(__PICK(buf, 1));
    tmp.prepend(__PICK(buf, 2));
    tmp.prepend(__PICK(buf, 3));
    tmp.prepend(0xa1);
    tmp.append(0x9b);
    tmp.append(0x84);
    buf = 1;
    tmp.append(__PICK(buf, 3));
    tmp.append(__PICK(buf, 2));
    tmp.append(__PICK(buf, 1));
    tmp.append(__PICK(buf, 0));
    buf = tmp.size() | 0x10000000;
    tmp.prepend(__PICK(buf, 0));
    tmp.prepend(__PICK(buf, 1));
    tmp.prepend(__PICK(buf, 2));
    tmp.prepend(__PICK(buf, 3));
    tmp.prepend(0xa0);
    bin_out.append(tmp);
    buf = (pts + timer.elapsed());
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    bin_out.prepend(__PICK(buf, 4));
    bin_out.prepend(__PICK(buf, 5));
    bin_out.prepend(__PICK(buf, 6));
    bin_out.prepend(__PICK(buf, 7));
    bin_out.prepend(0x88);
    bin_out.prepend(0xe7);
    buf = bin_out.size() | 0x0100000000000000;
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    bin_out.prepend(__PICK(buf, 4));
    bin_out.prepend(__PICK(buf, 5));
    bin_out.prepend(__PICK(buf, 6));
    bin_out.prepend(__PICK(buf, 7));
    buf = 0x1f43b675;
    bin_out.prepend(__PICK(buf, 0));
    bin_out.prepend(__PICK(buf, 1));
    bin_out.prepend(__PICK(buf, 2));
    bin_out.prepend(__PICK(buf, 3));
    fifo_file->write(bin_out);
    fifo_file->flush();
}

int DanmakuLauncher::getAvailDanmakuChannel(double speed)
{
    qint64 c_pts = timer.elapsed() + pts;
    for(int i = 0; i < channel_num; i++)
    {
        if (((double)(danmaku_channel[i].duration - c_pts + danmaku_channel[i].begin_pts) * speed / 1000.0) > 1920) {
            continue;
        } else {
            if (((double)(danmaku_channel[i].length + 1920) * (c_pts - danmaku_channel[i].begin_pts) / danmaku_channel[i].duration) < danmaku_channel[i].length ) {
                continue;
            } else {
                return i;
            }
        }
    }
    return -4;
}

void DanmakuLauncher::printPlayerProcess()
{
    while (player->canReadLine()) {
        QString err = player->readLine();
        qDebug() << err;
        if (err.contains("The specified session has been invalidated for some reason")) {
            qCritical() << "Stream invalid!";
            QCoreApplication::exit(1);
        }
    }
}

void DanmakuLauncher::getLiveStatus()
{
    bool online = false;
    while(live_checker->canReadLine()) {
        QString r = player->readAll();
        QRegularExpression re("^(http.+)$");
        QString line(live_checker->readLine());
        QRegularExpressionMatch match = re.match(line);
        if (match.hasMatch()) {
            online = true;
        }
    }
    if (online) {
        qDebug() << "Stream online.";
    } else {
        qCritical() << "Stream offline, closing...";
        QCoreApplication::exit(1);
    }

}

void DanmakuLauncher::launchLiveChecker()
{
//    qDebug() << live_checker->state();
    if (live_checker->state() == QProcess::NotRunning) {
        if (!live_checker->canReadLine()) {
            live_checker->start("ykdl", QStringList() << "-i" << url);
        }
    }
}

QString DanmakuLauncher::getStreamUrl(QString url)
{
    QProcess p;
    p.start("ykdl", QStringList() << "-i" << url);
    p.waitForStarted();
    p.waitForFinished();
    QRegularExpression re("^(http.+)$");
    QRegularExpression re_title("^title: +([^\n]+)$");
    while(!p.atEnd()) {
        QString line(p.readLine());
        QRegularExpressionMatch match = re_title.match(line);
        if (match.hasMatch()) {
             this->title = match.captured(1);
        }
        match = re.match(line);
        if (match.hasMatch()) {
             return match.captured(1);
        }
    }
    qCritical() << "Stream url not found!";
    return "null";
}


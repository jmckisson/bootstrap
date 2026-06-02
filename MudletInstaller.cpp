#include "MudletInstaller.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDebug>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QStandardPaths>
#include <QTimer>
#include <QVersionNumber>

QMap<QString, QString> getPlatformFeedMap(const QString &type) {

    const QString dblsqdFeedType = type == "PTB" ? "public-test-build" : "release";

    const QString dblsqdFeedUrl = "https://feeds.dblsqd.com/MKMMR7HNSP65PquQQbiDIw/";

    return {
        {"mac/arm",         QString("%1%2/mac/arm").arg(dblsqdFeedUrl).arg(dblsqdFeedType)},
        {"mac/x86_64",      QString("%1%2/mac/x86_64").arg(dblsqdFeedUrl).arg(dblsqdFeedType)},
        {"win/x86_64",      QString("%1%2/win/x86_64").arg(dblsqdFeedUrl).arg(dblsqdFeedType)},
        {"win/x86",         QString("%1%2/win/x86").arg(dblsqdFeedUrl).arg(dblsqdFeedType)},
        {"linux/x86_64",    QString("%1%2/linux/x86_64").arg(dblsqdFeedUrl).arg(dblsqdFeedType)}
    };
}


QString detectOS() {
    QString osKey;

#if defined(Q_OS_WIN)
    if (sizeof(void*) == 8) {
        osKey = "win/x86_64";
    } else {
        osKey = "win/x86";
    }
#elif defined(Q_OS_MAC)
    QString architecture = QSysInfo::currentCpuArchitecture();
    if (architecture.contains("arm64")) {
        osKey = "mac/arm";
    } else if (architecture.contains("x86_64")) {
        osKey = "mac/x86_64";
    }
#elif defined(Q_OS_LINUX)
    osKey = "linux/x86_64";  // Extend for other architectures as needed
#else
    osKey = "unknown";
#endif

    return osKey;
}


// Returns the launch.ini to read settings from. An external launch.ini sitting
// next to the executable takes precedence, which lets a single compiled binary
// be configured for a given game without recompiling/relinking. If no external
// file is present we fall back to the copy embedded in the Qt resources.
QString launchIniPath() {
    const QString external = QCoreApplication::applicationDirPath() + "/launch.ini";
    if (QFile::exists(external)) {
        return external;
    }
    return QStringLiteral(":/resources/launch.ini");
}


QString readLaunchProfileFromResource() {
    QSettings settings(launchIniPath(), QSettings::IniFormat);

    QString profile = settings.value("Settings/MUDLET_PROFILES", "").toString();

    if (profile.isEmpty()) {
        qDebug() << "MUDLET_PROFILES not found in resource file.";
    }

    return profile;
}


/**
 * @brief Verify the downloaded file sha256 with the provided hash from dblsqd
 * 
 * @param filePath Path to the file of whose hash wil be computed
 * @param expectedHash Expected sha256 hash
 * @return true If the expectedHash matches the sha256 hash of the file
 * @return false If the expectedHash does not match the sha256 hash of the file
 */
bool verifyFileSha256(const QString &filePath, const QString &expectedHash) {

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open file:" << filePath;
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        qDebug() << "Failed to compute SHA-256 hash.";
        return false;
    }

    file.close();

    QString hexHash = hash.result().toHex();

    if (hash.result().toHex().isEmpty()) {
        qDebug() << "SHA-256 computation failed.";
        return false;
    }

    if (hexHash.compare(expectedHash, Qt::CaseInsensitive) == 0) {
        qDebug() << "SHA-256 verification succeeded.";
        return true;
    } else {
        qDebug() << "SHA-256 verification failed.";
        qDebug() << "Computed:" << hexHash;
        qDebug() << "Expected:" << expectedHash;
        return false;
    }
}


MudletInstaller::MudletInstaller(QObject *parent) :
    QObject(parent),
    currentReply(nullptr),
    m_stateMachine(nullptr),
    retryCount(0),
    bytesAlreadyDownloaded(0) {

    // Read game name from launch profile
    gameName = readLaunchProfileFromResource();
    if (gameName.isEmpty()) {
        gameName = "your game";  // fallback if no game name is found
    }

    progressWindow = new QWidget;
    progressWindow->setWindowTitle("Downloading...");
    progressWindow->resize(400, 150);

    QVBoxLayout *layout = new QVBoxLayout(progressWindow);

    statusLabel = new QLabel("Preparing to download...", progressWindow);
    layout->addWidget(statusLabel);

    progressBar = new QProgressBar(progressWindow);
    progressBar->setRange(0, 100);
    layout->addWidget(progressBar);

    progressWindow->setLayout(layout);

    initStateMachine();
}


/**
 * @brief Initialize the QStateMachine
 * 
 */
void MudletInstaller::initStateMachine() {
    m_stateMachine = new QStateMachine(this);

    // Create states
    m_downloadFeedState = new QState(m_stateMachine);
    m_checkExistingState = new QState(m_stateMachine);
    m_downloadState = new QState(m_stateMachine);
    m_retryState = new QState(m_stateMachine);
    m_verifyHashState = new QState(m_stateMachine);
    m_installState = new QState(m_stateMachine);
    m_errorState = new QState(m_stateMachine);
    m_doneState = new QFinalState(m_stateMachine);

    // Set initial state
    m_stateMachine->setInitialState(m_downloadFeedState);

    // Connect state entry actions
    connect(m_downloadFeedState, &QState::entered, this, &MudletInstaller::fetchPlatformFeed);
    connect(m_checkExistingState, &QState::entered, this, &MudletInstaller::checkExistingFile);
    connect(m_downloadState, &QState::entered, this, &MudletInstaller::startDownload);
    connect(m_retryState, &QState::entered, this, &MudletInstaller::retryDownload);
    connect(m_verifyHashState, &QState::entered, this, &MudletInstaller::verifyHash);
    connect(m_installState, &QState::entered, this, &MudletInstaller::installApplication);
    connect(m_errorState, &QState::entered, this, &MudletInstaller::handleError);
    connect(m_doneState, &QState::entered, this, &MudletInstaller::cleanup);

    // Add state transitions
    m_downloadFeedState->addTransition(this, &MudletInstaller::feedFetched, m_checkExistingState);
    m_downloadFeedState->addTransition(this, &MudletInstaller::errorOccurred, m_errorState);

    m_checkExistingState->addTransition(this, &MudletInstaller::fileExists, m_verifyHashState);
    m_checkExistingState->addTransition(this, &MudletInstaller::fileNotExists, m_downloadState);

    m_downloadState->addTransition(this, &MudletInstaller::downloadComplete, m_verifyHashState);
    m_downloadState->addTransition(this, &MudletInstaller::errorOccurred, m_retryState);

    m_retryState->addTransition(this, &MudletInstaller::fileNotExists, m_downloadState);  // retry download
    m_retryState->addTransition(this, &MudletInstaller::errorOccurred, m_errorState);     // max retries reached

    m_verifyHashState->addTransition(this, &MudletInstaller::hashValid, m_installState);
    m_verifyHashState->addTransition(this, &MudletInstaller::hashInvalid, m_errorState);

    m_installState->addTransition(this, &MudletInstaller::installComplete, m_doneState);
    m_installState->addTransition(this, &MudletInstaller::errorOccurred, m_errorState);

    m_errorState->addTransition(this, &MudletInstaller::finished, m_doneState);

    // Connect state machine finished signal
    connect(m_stateMachine, &QStateMachine::finished, this, [this]() {
        qDebug() << "State machine finished";
        progressWindow->close();
    });

    // Debug state transitions
    connect(m_downloadFeedState, &QState::entered, this, []() { qDebug() << "Entered: DownloadFeed"; });
    connect(m_downloadFeedState, &QState::exited, this, []() { qDebug() << "Exited: DownloadFeed"; });

    connect(m_checkExistingState, &QState::entered, this, []() { qDebug() << "Entered: CheckExisting"; });
    connect(m_checkExistingState, &QState::exited, this, []() { qDebug() << "Exited: CheckExisting"; });

    connect(m_downloadState, &QState::entered, this, []() { qDebug() << "Entered: Download"; });
    connect(m_downloadState, &QState::exited, this, []() { qDebug() << "Exited: Download"; });

    connect(m_retryState, &QState::entered, this, []() { qDebug() << "Entered: Retry"; });
    connect(m_retryState, &QState::exited, this, []() { qDebug() << "Exited: Retry"; });

    connect(m_verifyHashState, &QState::entered, this, []() { qDebug() << "Entered: VerifyHash"; });
    connect(m_verifyHashState, &QState::exited, this, []() { qDebug() << "Exited: VerifyHash"; });

    connect(m_installState, &QState::entered, this, []() { qDebug() << "Entered: Install"; });
    connect(m_installState, &QState::exited, this, []() { qDebug() << "Exited: Install"; });

    connect(m_errorState, &QState::entered, this, []() { qDebug() << "Entered: Error"; });
    connect(m_errorState, &QState::exited, this, []() { qDebug() << "Exited: Error"; });

    connect(m_doneState, &QState::entered, this, []() { qDebug() << "Entered: Done"; });
}


/**
 * @brief Start the state machine
 * 
 */
void MudletInstaller::start() {
    m_stateMachine->start();
}


/**
 * @brief Query the platform OS and fetch the proper platform feed from dblsqd
 */
void MudletInstaller::fetchPlatformFeed() {

    QSettings settings(launchIniPath(), QSettings::IniFormat);

    QString releaseType = settings.value("Settings/RELEASE_TYPE", "").toString();

    QMap<QString, QString> feedMap = getPlatformFeedMap(releaseType);

    QString os = detectOS();

    QString feedUrl = feedMap.value(os);

    if (feedUrl.isEmpty()) {
        qDebug() << "No feed URL found for platform:" << os;
        emit errorOccurred();
        return;
    }

    currentReply = networkManager.get(QNetworkRequest(QUrl(feedUrl)));

    connect(currentReply, &QNetworkReply::finished, this, &MudletInstaller::onFetchPlatformFeedFinished);

    // Show the progress bar window
    progressWindow->show();
}

/**
 * @brief Called upon complete receipt of the platform feed. 
 * Extracts the url and sha256 from the JSON and sets up a new download for the proper file.
 */
void MudletInstaller::onFetchPlatformFeedFinished() {
    if (currentReply->error() != QNetworkReply::NoError) {
        qDebug() << "Error fetching feed:" << currentReply->errorString();
        currentReply->deleteLater();
        emit errorOccurred();
        return;
    }

    QByteArray jsonData = currentReply->readAll();
    currentReply->deleteLater();

    QJsonDocument doc = QJsonDocument::fromJson(jsonData);
    if (doc.isNull() || !doc.isObject()) {
        qDebug() << "Invalid JSON data.";
        emit errorOccurred();
        return;
    }

    QJsonObject rootObj = doc.object();
    QJsonArray releases = rootObj.value("releases").toArray();
    if (releases.isEmpty()) {
        qDebug() << "No releases found.";
        emit errorOccurred();
        return;
    }

    QJsonObject firstRelease = releases[0].toObject();
    QJsonObject download = firstRelease.value("download").toObject();
    info.sha256 = download.value("sha256").toString();
    info.url = download.value("url").toString();

    qDebug() << "SHA-256:" << info.sha256;
    qDebug() << "URL:" << info.url;

    QRegularExpression regex(R"(/([^/]+)\.(exe|dmg|AppImage\.tar)$)");
    QRegularExpressionMatch match = regex.match(info.url);

    if (match.hasMatch()) {
        QString os = detectOS();
        
        info.appName = match.captured(1);
        if (os.startsWith("mac") || os.startsWith("linux")) {
            info.appName += "." + match.captured(2);
        }
    } else {
        qDebug() << "No match found in URL:" << info.url;
        emit errorOccurred();
        return;
    }

    outputFile = info.appName;

    QString osStr = detectOS();
    // Mac may have an issue downloading a file into the .app directory
    if (osStr.startsWith("mac")) {
        outputFile = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + outputFile;
    }

    qDebug() << "OutputFile: " << outputFile;

    emit feedFetched();
}


/**
 * @brief Check if outputFile exists and emit corresponding state signal
 * Also determines bytes already downloaded for resume functionality
 */
void MudletInstaller::checkExistingFile() {
    statusLabel->setText("Checking existing file...");
    qDebug() << "Checking if file exists:" << outputFile;

    if (QFile::exists(outputFile)) {
        QFile file(outputFile);
        bytesAlreadyDownloaded = file.size();
        qDebug() << outputFile << "exists, size:" << bytesAlreadyDownloaded << "bytes";
        
        // If we have some bytes but not a complete file, we might want to resume
        // For now, treat any existing file as complete and verify hash
        emit fileExists();
    } else {
        bytesAlreadyDownloaded = 0;
        qDebug() << outputFile << "does not exist";
        emit fileNotExists();
    }
}


/**
 * @brief Create a request and start downloading the Mudlet installer
 * Supports resuming downloads using HTTP Range requests
 */
void MudletInstaller::startDownload() {
    QNetworkRequest request{QUrl(info.url)};
    
    // If we have bytes already downloaded, request only the remaining part
    if (bytesAlreadyDownloaded > 0) {
        QString rangeHeader = QString("bytes=%1-").arg(bytesAlreadyDownloaded);
        request.setRawHeader("Range", rangeHeader.toUtf8());
        qDebug() << "Resuming download from byte:" << bytesAlreadyDownloaded;
        statusLabel->setText(QString("Resuming %1... (attempt %2/%3)")
            .arg(info.appName)
            .arg(retryCount + 1)
            .arg(MAX_RETRIES + 1));
    } else {
        statusLabel->setText(QString("Downloading Mudlet for %1...").arg(gameName));
    }
    
    currentReply = networkManager.get(request);

    connect(currentReply, &QNetworkReply::downloadProgress, this, &MudletInstaller::onDownloadProgress);
    connect(currentReply, &QNetworkReply::finished, this, &MudletInstaller::onDownloadFinished);
    connect(currentReply, &QNetworkReply::errorOccurred, this, &MudletInstaller::onDownloadError);

}

void MudletInstaller::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int progress = static_cast<int>((bytesReceived * 100) / bytesTotal);
        progressBar->setValue(progress);
        
        // Set MB progress text on the progress bar
        progressBar->setFormat(QString("%1 / %2 MB")
            .arg(bytesReceived/1048576.0, 0, 'f', 2)
            .arg(bytesTotal/1048576.0, 0, 'f', 2));
    }
    statusLabel->setText(QString("Downloading Mudlet for %1...").arg(gameName));
}


/**
 * @brief Verifies the sha256 hash and starts the install process if the hash matches what we got
 * from the dblsqd feed.
 * Called upon completion of the Mudlet installer download.
 * Supports appending to existing file when resuming.
 */
void MudletInstaller::onDownloadFinished() {
    if (currentReply->error() != QNetworkReply::NoError) {
        statusLabel->setText(QString("Error downloading file: %1").arg(currentReply->errorString()));
        emit errorOccurred();
        return;
    }

    qDebug() << "Download finished: " << outputFile;

    QFile file(outputFile);
    QIODevice::OpenMode openMode = (bytesAlreadyDownloaded > 0) ? 
        (QIODevice::WriteOnly | QIODevice::Append) : QIODevice::WriteOnly;
    
    if (file.open(openMode)) {
        file.write(currentReply->readAll());
        file.close();
        qDebug() << "Downloaded to:" << outputFile;

        // Reset retry count and bytes already downloaded on successful completion
        retryCount = 0;
        bytesAlreadyDownloaded = 0;
        
        emit downloadComplete();

    } else {
        qDebug() << "Failed to save file.";
        emit errorOccurred();
    }

    currentReply->deleteLater();
}


void MudletInstaller::onDownloadError(QNetworkReply::NetworkError error) {
    // Capture error string before deleteLater to avoid use-after-free
    QString errorString = currentReply->errorString();
    qDebug() << "Download error:" << errorString << "Error code:" << error;

    // Distinguish between retryable and non-retryable errors
    bool isRetryable = true;

    switch (error) {
        case QNetworkReply::ContentNotFoundError:      // 404
        case QNetworkReply::AuthenticationRequiredError: // 401
        case QNetworkReply::ContentAccessDenied:       // 403
        case QNetworkReply::ProtocolFailure:           // Invalid response
            isRetryable = false;
            qDebug() << "Non-retryable error detected";
            break;
        case QNetworkReply::ConnectionRefusedError:
        case QNetworkReply::RemoteHostClosedError:
        case QNetworkReply::HostNotFoundError:
        case QNetworkReply::TimeoutError:
        case QNetworkReply::OperationCanceledError:
        case QNetworkReply::TemporaryNetworkFailureError:
        case QNetworkReply::NetworkSessionFailedError:
        default:
            isRetryable = true;
            qDebug() << "Retryable error detected";
            break;
    }

    currentReply->deleteLater();

    if (!isRetryable) {
        statusLabel->setText(QString("Download failed: %1").arg(errorString));
        // Skip retry logic and go directly to error state for non-retryable errors
        retryCount = MAX_RETRIES; // This will force retryDownload to give up immediately
    }

    emit errorOccurred();
}


/**
 * @brief Verify the hash of outputFile with the provided sha256
 * Emits corfresponding hashValid or hashInvalid state signals
 * 
 */
void MudletInstaller::verifyHash() {
    statusLabel->setText("Verifying SHA256...");
    statusLabel->repaint();
    qDebug() << "Verifying hash";

    if (!verifyFileSha256(outputFile, info.sha256)) {
        qDebug() << "Checksum verification failed.";
        statusLabel->setText("SHA256 Verification Failed");
        emit hashInvalid();
    } else {
        qDebug() << "Checksum verification succeeded.";
        emit hashValid();
    }
}


/**
 * @brief Windows install process
 * 
 * @param env 
 * @param exeFilePath
 * @param shortcutCreated
 * @return true If Mudlet was downloaded and installed
 * @return false If any errors during this process
 */
bool installAndRunExe(QProcessEnvironment &env, const QString& exeFilePath, QString& gameName, bool &shortcutCreated) {

    QProcess process;

    process.setProcessEnvironment(env);
    process.start("cmd.exe", {"/C", exeFilePath});
    bool success = process.waitForFinished();

    if (!success) {
        return false;
    }

    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString originalShortcut = QDir(desktopPath).absoluteFilePath("Mudlet.lnk");
    QString newShortcut = QDir(desktopPath).absoluteFilePath(QString("%1.lnk").arg(gameName));
    
    // Check if original exists
    if (!QFile::exists(originalShortcut)) {
        qDebug() << "Original Mudlet shortcut not found";
        return true;
    }
    
    // Don't copy if it already exists
    if (!QFile::exists(newShortcut)) {
        // Copy the shortcut
        shortcutCreated = QFile::copy(originalShortcut, newShortcut);
        if (shortcutCreated) {
            qDebug() << "Created game shortcut:" << newShortcut;
        } else {
            qDebug() << "Failed to create game shortcut";
        }
    }

    return true;
}


/**
 * @brief macOS install process
 * 
 * @param env 
 * @param dmgFilePath 
 * @return true If the dmg was properly mounted, ran, Mudlet.app copied, and dmg cleaned up
 * @return false If any errors during this process
 */
bool installAndRunDmg(QProcessEnvironment &env, const QString& dmgFilePath) {
    QProcess process;

    // Mount the .dmg file
    QString mountPoint;
    process.start("hdiutil", {"attach", dmgFilePath, "-nobrowse"});
    process.waitForFinished();
    QString output = process.readAllStandardOutput();
    qDebug() << output;

    // Extract the mount point (assumes it's in the last line of the output)
    QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (line.contains("/Volumes/")) {
            mountPoint = line.section('\t', -1);
            break;
        }
    }
    if (mountPoint.isEmpty()) {
        qWarning() << "Failed to mount .dmg.";
        return false;
    }
    qDebug() << "Mounted at:" << mountPoint;

    // Copy the application to ~/Applications
    QString appName = "Mudlet.app";
    QString appPath = mountPoint + "/" + appName;
    QString targetDir = QDir::homePath() + "/Applications/";
    QString targetAppPath = targetDir + appName;

    if (QFile::exists(targetAppPath)) {
        qDebug() << "Application already exists at" << targetAppPath << ". Removing it...";
        if (!QFile::remove(targetAppPath)) {
            qWarning() << "Failed to remove existing application, trying recursive delete...";
            if (!QDir(targetAppPath).removeRecursively()) {
                qWarning() << "Failed to recursively remove existing application.";
                process.start("rm", {"-rf", targetAppPath});
                process.waitForFinished();
                if (process.exitCode() != 0) {
                    qWarning() << "Failed to remove application:" << process.readAllStandardError();
                    return false;
                }
            }
            
        }
        qDebug() << "Existing application removed successfully.";
    }

    QDir().mkpath(targetDir); // Ensure the Applications folder exists
    process.start("cp", {"-R", appPath, targetDir});
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to copy application:" << process.readAllStandardError();
        return false;
    }
    qDebug() << "Application copied to" << targetDir;

    // Unmount the .dmg
    process.start("hdiutil", {"detach", mountPoint});
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to unmount .dmg:" << process.readAllStandardError();
        return false;
    }
    qDebug() << ".dmg unmounted successfully.";

    // Run the application
    QString appExecutable = targetDir + "/Mudlet.app";
    process.setProcessEnvironment(env);
    process.start("open", {appExecutable});
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to launch application:" << process.readAllStandardError();
        return false;
    }
    qDebug() << "Application launched successfully.";

    return true;
}


/**
 * @brief Liknux install process
 * 
 * @param env 
 * @param tarFilePath 
 * @return true 
 * @return false 
 */
bool installAndRunAppImage(QProcessEnvironment &env, const QString& tarFilePath) {
    QProcess process;

    // Extract the tar file
    QString extractDir = QDir::tempPath() + "/ExtractedApp"; // Temporary directory for extraction
    QDir().mkpath(extractDir); // Ensure the directory exists
    process.start("tar", {"-xf", tarFilePath, "-C", extractDir});
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to extract tar file:" << process.readAllStandardError();
        return false;
    }
    qDebug() << "Tar file extracted to" << extractDir;

    // Locate the AppImage file
    QDir dir(extractDir);
    QStringList appImages = dir.entryList({"*.AppImage"}, QDir::Files);
    if (appImages.isEmpty()) {
        qWarning() << "No AppImage file found in the extracted directory.";
        return false;
    }
    QString appImagePath = dir.filePath(appImages.first());
    qDebug() << "Found AppImage:" << appImagePath;

    // Make the AppImage executable
    process.start("chmod", {"+x", appImagePath});
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to make AppImage executable:" << process.readAllStandardError();
        return false;
    }
    qDebug() << "AppImage is now executable.";

    // Run the AppImage
    process.setProcessEnvironment(env);
    process.start(appImagePath);
    process.waitForFinished();
    if (process.exitCode() != 0) {
        qWarning() << "Failed to run AppImage:" << process.readAllStandardError();
        return false;
    }
    qDebug() << "AppImage launched successfully.";

    return true;
}


/**
 * @brief Platform agnostic install steps.
 * - Sets up MUDLET_PROFILES env variable from launch.ini
 * - Installs autologin file for the wanted profile in users home/.config/Mudlet/<profile>
 * - runs the platform specific installer.
 * 
 * Emits corresponding installComplete or errorOccurred state signal.
 * 
 */
void MudletInstaller::installApplication() {

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // Read the profile from the .ini file
    QString launchProfile = readLaunchProfileFromResource();
    if (launchProfile.isEmpty()) {
        qDebug() << "No launch profile found. Using default.";
    } else {
        // Pass along the launch profile to the environment
        env.insert("MUDLET_PROFILES", launchProfile);
    }

    // Create autologin file for the wanted profile
    QString confDirDefault = QDir::homePath() + 
        QDir::separator() + ".config" +
        QDir::separator() + "mudlet" +
        QDir::separator() + "profiles" + 
        QDir::separator() + launchProfile;
    QDir configDir;
    if (!configDir.mkpath(confDirDefault)) {
        qDebug() << "Failed to create config directory:" << confDirDefault;
    } else {
        // Create the autologin file
        QString autologinFilePath = confDirDefault + QDir::separator() + "autologin";
        QFile autologinFile(autologinFilePath);

        // A constant equivalent to QDataStream::Qt_5_12 needed in several places
        // which can't be pulled from Qt as it is not going to be defined for older
        // versions:
        static const int scmQDataStreamFormat_5_12 = 18;

        if (autologinFile.open(QIODevice::WriteOnly)) {
            QDataStream out(&autologinFile);
            
            // Set the same data stream version that Mudlet uses for reading
            if (QVersionNumber::fromString(qVersion()) >= QVersionNumber(5, 13, 0)) {
                out.setVersion(scmQDataStreamFormat_5_12);
            }

            QString autologinData = QString::number(Qt::Checked);
            out << autologinData;
            
            autologinFile.close();
            qDebug() << "Autologin file created successfully:" << autologinFilePath;
        } else {
            qWarning() << "Failed to create autologin file:" << autologinFilePath << autologinFile.errorString();
        }
    }

    statusLabel->setText(QString("Installing %1").arg(info.appName));
    statusLabel->repaint();

    bool installSuccess = false;
    
    // Install the application
#if defined(Q_OS_WIN)
    bool shortcutCreated = false;
    installSuccess = installAndRunExe(env, outputFile, launchProfile, shortcutCreated);
#elif defined(Q_OS_MAC)
    installSuccess = installAndRunDmg(env, outputFile);
#elif defined(Q_OS_LINUX)
    installSuccess = installAndRunAppImage(env, outputFile);
#endif

    if (!installSuccess) {
        emit errorOccurred();
    } else {
        QString labelString = QString("%1 has been installed! You can now delete the MudletInstaller-%1 app.").arg(launchProfile);

#if defined(Q_OS_WIN)
        if (shortcutCreated) {
            labelString += QString("\nYou may use the Mudlet or the %1 desktop icons to play").arg(launchProfile);
        }
#endif

        statusLabel->setText(labelString);
        statusLabel->repaint();

        QTimer::singleShot(5000, [this]() {
            emit installComplete();
        });
    }
    
}


/**
 * @brief Simple error handler.
 * For now, just set the status label to a generic error message.
 * 
 */
void MudletInstaller::handleError() {
    qDebug() << "Handling error state";
    statusLabel->setText("An error occurred");
    // Show error dialog?
    emit finished();
}


/**
 * @brief Final state machine step. Deletes the downloaded file.
 * 
 */
void MudletInstaller::cleanup() {
    qDebug() << "Cleaning up";
    if (QFile::exists(outputFile)) {
        if (!QFile::remove(outputFile)) {
            qDebug() << "Error removing" << outputFile << "during cleanup";
        } else {
            qDebug() << "Removed" << outputFile;
        }
    }
    progressWindow->close();
}


/**
 * @brief Handle download retry logic
 * Decides whether to retry the download or give up based on retry count
 */
void MudletInstaller::retryDownload() {
    qDebug() << "Retry state entered, retry count:" << retryCount;
    
    if (retryCount < MAX_RETRIES) {
        retryCount++;
        statusLabel->setText(QString("Retrying download... (attempt %1/%2)")
            .arg(retryCount + 1)
            .arg(MAX_RETRIES + 1));
        statusLabel->repaint();
        
        qDebug() << "Attempting retry" << retryCount << "of" << MAX_RETRIES;
        
        // Short delay before retrying
        QTimer::singleShot(1000, [this]() {
            emit fileNotExists(); // This will trigger transition back to download state
        });
    } else {
        qDebug() << "Max retries reached, giving up";
        statusLabel->setText("Download failed after maximum retries");
        emit errorOccurred(); // This will trigger transition to error state
    }
}

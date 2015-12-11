/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * This file incorporates work covered by the following license notice:
 *
 *   Licensed to the Apache Software Foundation (ASF) under one or more
 *   contributor license agreements. See the NOTICE file distributed
 *   with this work for additional information regarding copyright
 *   ownership. The ASF licenses this file to you under the Apache
 *   License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of
 *   the License at http://www.apache.org/licenses/LICENSE-2.0 .
 */

#include "KDE5FilePicker.hxx"

#include <com/sun/star/lang/DisposedException.hpp>
#include <com/sun/star/lang/XMultiServiceFactory.hpp>
#include <cppuhelper/interfacecontainer.h>
#include <cppuhelper/supportsservice.hxx>
#include <com/sun/star/ui/dialogs/TemplateDescription.hpp>
#include <com/sun/star/ui/dialogs/CommonFilePickerElementIds.hpp>
#include <com/sun/star/ui/dialogs/ExtendedFilePickerElementIds.hpp>
#include <com/sun/star/ui/dialogs/ControlActions.hpp>
#include <com/sun/star/ui/dialogs/ExecutableDialogResults.hpp>

#include <osl/mutex.hxx>

#include <vcl/fpicker.hrc>
#include <vcl/svapp.hxx>
#include <vcl/sysdata.hxx>
#include <vcl/syswin.hxx>

#include "osl/file.h"

#include "FPServiceInfo.hxx"
#include "VCLKDEApplication.hxx"
/*
#include <kfilefiltercombo.h>
#include <kfilewidget.h>
#include <kdiroperator.h>
#include <kservicetypetrader.h>
#include <kmessagebox.h>
*/

#include <kwindowsystem.h>

// #include <QtCore/QDebug>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QWidget>
#include <QtCore/QThread>

#undef Region

#include "generic/geninst.h"

#include "svids.hrc"

using namespace ::com::sun::star;
using namespace ::com::sun::star::ui::dialogs;
using namespace ::com::sun::star::ui::dialogs::TemplateDescription;
using namespace ::com::sun::star::ui::dialogs::ExtendedFilePickerElementIds;
using namespace ::com::sun::star::ui::dialogs::CommonFilePickerElementIds;
using namespace ::com::sun::star::lang;
using namespace ::com::sun::star::beans;
using namespace ::com::sun::star::uno;

// The dialog should check whether LO also supports the protocol
// provided by KIO, and KFileWidget::dirOperator() is only 4.3+ .
// Moreover it's only in this somewhat internal KFileWidget class,
// which may not necessarily be what QFileDialog::fileWidget() returns,
// but that's hopefully not a problem in practice.
//#if KDE_VERSION_MAJOR == 4 && KDE_VERSION_MINOR >= 2
//#define ALLOW_REMOTE_URLS 1
//#else
#define ALLOW_REMOTE_URLS 0
//#endif

// helper functions


namespace
{
    uno::Sequence<OUString> SAL_CALL FilePicker_getSupportedServiceNames()
    {
        uno::Sequence<OUString> aRet(3);
        aRet[0] = "com.sun.star.ui.dialogs.FilePicker";
        aRet[1] = "com.sun.star.ui.dialogs.SystemFilePicker";
        aRet[2] = "com.sun.star.ui.dialogs.KDE5FilePicker";
        return aRet;
    }
}

OUString toOUString(const QString& s)
{
    // QString stores UTF16, just like OUString
    return OUString(reinterpret_cast<const sal_Unicode*>(s.data()), s.length());
}

QString toQString(const OUString& s)
{
    return QString::fromUtf16(
        reinterpret_cast<ushort const *>(s.getStr()), s.getLength());
}

// KDE5FilePicker

KDE5FilePicker::KDE5FilePicker( const uno::Reference<uno::XComponentContext>& )
    : KDE5FilePicker_Base(_helperMutex)
    , allowRemoteUrls( false )
{
    _extraControls = new QWidget();
    _layout = new QGridLayout(_extraControls);

    _dialog = new QFileDialog(nullptr, QString(""), QString("~"));
//   _extraControls);
#if ALLOW_REMOTE_URLS
    if( KFileWidget* fileWidget = dynamic_cast< KFileWidget* >( _dialog->fileWidget()))
    {
        allowRemoteUrls = true;
        // Use finishedLoading signal rather than e.g. urlEntered, because if there's a problem
        // such as the URL being mistyped, there's no way to prevent two message boxes about it,
        // one from us and one from KDE code.
        connect( fileWidget->dirOperator(), SIGNAL( finishedLoading()), SLOT( checkProtocol()));
    }
#endif

    setMultiSelectionMode( false );

    // XExecutableDialog functions
    connect( this, SIGNAL( setTitleSignal( const OUString & ) ),
             this, SLOT( setTitleSlot( const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( executeSignal() ),
             this, SLOT( executeSlot() ), Qt::BlockingQueuedConnection );

    // XFilePicker functions
    connect( this, SIGNAL( setMultiSelectionModeSignal( bool ) ),
             this, SLOT( setMultiSelectionModeSlot( bool ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( setDefaultNameSignal( const OUString & ) ),
             this, SLOT( setDefaultNameSlot( const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( setDisplayDirectorySignal( const OUString & ) ),
             this, SLOT( setDisplayDirectorySlot( const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( getDisplayDirectorySignal() ),
             this, SLOT( getDisplayDirectorySlot() ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( getFilesSignal() ),
             this, SLOT( getFilesSlot() ), Qt::BlockingQueuedConnection );

    // XFilterManager functions
    connect( this, SIGNAL( appendFilterSignal( const OUString &, const OUString & ) ),
             this, SLOT( appendFilterSlot( const OUString &, const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( setCurrentFilterSignal( const OUString & ) ),
             this, SLOT( setCurrentFilterSlot( const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( getCurrentFilterSignal() ),
             this, SLOT( getCurrentFilterSlot() ), Qt::BlockingQueuedConnection );

    // XFilterGroupManager functions
    connect( this, SIGNAL( appendFilterGroupSignal( const OUString &, const css::uno::Sequence< css::beans::StringPair > & ) ),
             this, SLOT( appendFilterGroupSlot( const OUString &, const css::uno::Sequence< css::beans::StringPair > & ) ), Qt::BlockingQueuedConnection );

    // XFilePickerControlAccess functions
    connect( this, SIGNAL( setValueSignal( sal_Int16, sal_Int16, const css::uno::Any & ) ),
             this, SLOT( setValueSlot( sal_Int16, sal_Int16, const css::uno::Any & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( getValueSignal( sal_Int16, sal_Int16 ) ),
             this, SLOT( getValueSlot( sal_Int16, sal_Int16 ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( enableControlSignal( sal_Int16, bool ) ),
             this, SLOT( enableControlSlot( sal_Int16, bool ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( setLabelSignal( sal_Int16, const OUString & ) ),
             this, SLOT( setLabelSlot( sal_Int16, const OUString & ) ), Qt::BlockingQueuedConnection );
    connect( this, SIGNAL( getLabelSignal( sal_Int16 ) ),
             this, SLOT( getLabelSlot( sal_Int16 ) ), Qt::BlockingQueuedConnection );

    // XFilePicker2 functions
    connect( this, SIGNAL( getSelectedFilesSignal() ),
             this, SLOT( getSelectedFilesSlot() ), Qt::BlockingQueuedConnection );

    // XInitialization
    connect( this, SIGNAL( initializeSignal( const css::uno::Sequence< css::uno::Any > & ) ),
             this, SLOT( initializeSlot( const css::uno::Sequence< css::uno::Any > & ) ), Qt::BlockingQueuedConnection );

    // Destructor proxy
    connect( this, SIGNAL( cleanupProxySignal() ), this, SLOT( cleanupProxy() ), Qt::BlockingQueuedConnection );

    connect( this, SIGNAL( checkProtocolSignal() ), this, SLOT( checkProtocol() ), Qt::BlockingQueuedConnection );

    // XFilePickerListener notifications
    connect( _dialog, SIGNAL( filterChanged(const QString&) ), this, SLOT( filterChanged(const QString&) ));
    connect( _dialog, SIGNAL( selectionChanged() ), this, SLOT( selectionChanged() ));
}

KDE5FilePicker::~KDE5FilePicker()
{
    cleanupProxy();
}

void KDE5FilePicker::cleanupProxy()
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser aReleaser;
        return Q_EMIT cleanupProxySignal();
    }
    delete _dialog;
}

void SAL_CALL KDE5FilePicker::addFilePickerListener( const uno::Reference<XFilePickerListener>& xListener )
    throw( uno::RuntimeException, std::exception )
{
    SolarMutexGuard aGuard;
    m_xListener = xListener;
}

void SAL_CALL KDE5FilePicker::removeFilePickerListener( const uno::Reference<XFilePickerListener>& )
    throw( uno::RuntimeException, std::exception )
{
    SolarMutexGuard aGuard;
    m_xListener.clear();
}

void SAL_CALL KDE5FilePicker::setTitle( const OUString &title )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser aReleaser;
        return Q_EMIT setTitleSignal( title );
    }

    _dialog->setWindowTitle(toQString(title));
}

sal_Int16 SAL_CALL KDE5FilePicker::execute()
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser aReleaser;
        return Q_EMIT executeSignal();
    }

    //get the window id of the main OO window to set it for the dialog as a parent
    vcl::Window *pParentWin = Application::GetDefDialogParent();
    if ( pParentWin )
    {
        const SystemEnvData* pSysData = static_cast<SystemWindow *>(pParentWin)->GetSystemData();
        if ( pSysData )
        {
            KWindowSystem::setMainWindow( _dialog, pSysData->aWindow); // unx only
        }
    }
/*
    _dialog->clearFilter();
    _dialog->setFilter(_filter);

    if(!_currentFilter.isNull())
        _dialog->filterWidget()->setCurrentItem(_currentFilter);
    _dialog->filterWidget()->setEditable(false);
*/

    VCLKDEApplication::preDialogSetup();
    //block and wait for user input
    int result = _dialog->exec();
    VCLKDEApplication::postDialogCleanup();
    if( result == QFileDialog::Accepted )
        return ExecutableDialogResults::OK;

    return ExecutableDialogResults::CANCEL;
}

void SAL_CALL KDE5FilePicker::setMultiSelectionMode( sal_Bool multiSelect )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setMultiSelectionModeSignal( multiSelect );
    }

    if (multiSelect)
        _dialog->setFileMode(QFileDialog::ExistingFiles);
    else
        _dialog->setFileMode(QFileDialog::ExistingFile);
}

void SAL_CALL KDE5FilePicker::setDefaultName( const OUString &name )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setDefaultNameSignal( name );
    }

    _dialog->selectUrl(QUrl(toQString(name)));
}

void SAL_CALL KDE5FilePicker::setDisplayDirectory( const OUString &dir )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setDisplayDirectorySignal( dir );
    }

    _dialog->selectUrl(QUrl(toQString(dir)));
}

OUString SAL_CALL KDE5FilePicker::getDisplayDirectory()
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getDisplayDirectorySignal();
    }

    return toOUString(_dialog->directoryUrl().url());
}

uno::Sequence< OUString > SAL_CALL KDE5FilePicker::getFiles()
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getFilesSignal();
    }
    uno::Sequence< OUString > seq = getSelectedFiles();
    if (seq.getLength() > 1)
        seq.realloc(1);
    return seq;
}

uno::Sequence< OUString > SAL_CALL KDE5FilePicker::getSelectedFiles()
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getSelectedFilesSignal();
    }
    const QList<QUrl> urls = _dialog->selectedUrls();
    uno::Sequence< OUString > seq( urls.size() );
    int i = 0;
    foreach( const QUrl& url, urls )
        seq[ i++ ]= toOUString( url.url());
    return seq;
}

void SAL_CALL KDE5FilePicker::appendFilter( const OUString &title, const OUString &filter )
    throw( lang::IllegalArgumentException, uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT appendFilterSignal( title, filter );
    }

    QString t = toQString(title);
    QString f = toQString(filter);

    // '/' need to be escaped else they are assumed to be mime types by kfiledialog
    //see the docs
    t.replace("/", "\\/");

    // openoffice gives us filters separated by ';' qt dialogs just want space separated
    f.replace(";", " ");

    // make sure "*.*" is not used as "all files"
    f.replace("*.*", "*");

    _filters << QString("%1 (%2)").arg(f).arg(t);
}

void SAL_CALL KDE5FilePicker::setCurrentFilter( const OUString &title )
    throw( lang::IllegalArgumentException, uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setCurrentFilterSignal( title );
    }

    _currentFilter = toQString(title);
}

OUString SAL_CALL KDE5FilePicker::getCurrentFilter()
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getCurrentFilterSignal();
    }

    // _dialog->currentFilter() wouldn't quite work, because it returns only e.g. "*.doc",
    // without the description, and there may be several filters with the same pattern
    QString filter = _dialog->selectedNameFilter();
    filter = filter.mid( filter.indexOf( '|' ) + 1 ); // convert from the pattern|description format if needed
    filter.replace( "\\/", "/" );

    //default if not found
    if (filter.isNull())
        filter = "ODF Text Document (.odt)";

    return toOUString(filter);
}

void SAL_CALL KDE5FilePicker::appendFilterGroup( const OUString& rGroupTitle, const uno::Sequence<beans::StringPair>& filters)
    throw( lang::IllegalArgumentException, uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT appendFilterGroupSignal( rGroupTitle, filters );
    }

    const sal_uInt16 length = filters.getLength();
    for (sal_uInt16 i = 0; i < length; ++i)
    {
        beans::StringPair aPair = filters[i];
        appendFilter( aPair.First, aPair.Second );
    }
}

void SAL_CALL KDE5FilePicker::setValue( sal_Int16 controlId, sal_Int16 nControlAction, const uno::Any &value )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setValueSignal( controlId, nControlAction, value );
    }

    if (_customWidgets.contains( controlId )) {
        QCheckBox* cb = dynamic_cast<QCheckBox*>( _customWidgets.value( controlId ));
        if (cb)
            cb->setChecked(value.get<bool>());
    }
    else
        OSL_TRACE( "set label on unknown control %d", controlId );
}

uno::Any SAL_CALL KDE5FilePicker::getValue( sal_Int16 controlId, sal_Int16 nControlAction )
    throw( uno::RuntimeException, std::exception )
{
    if (CHECKBOX_AUTOEXTENSION == controlId)
        // We ignore this one and rely on QFileDialog to provide the function.
        // Always return false, to pretend we do not support this, otherwise
        // LO core would try to be smart and cut the extension in some places,
        // interfering with QFileDialog's handling of it. QFileDialog also
        // saves the value of the setting, so LO core is not needed for that either.
        return uno::Any( false );

    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getValueSignal( controlId, nControlAction );
    }

    uno::Any res(false);
    if (_customWidgets.contains( controlId )) {
        QCheckBox* cb = dynamic_cast<QCheckBox*>( _customWidgets.value( controlId ));
        if (cb)
            res = uno::Any(cb->isChecked());
    }
    else
        OSL_TRACE( "get value on unknown control %d", controlId );

    return res;
}

void SAL_CALL KDE5FilePicker::enableControl( sal_Int16 controlId, sal_Bool enable )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT enableControlSignal( controlId, enable );
    }

    if (_customWidgets.contains( controlId ))
        _customWidgets.value( controlId )->setEnabled( enable );
    else
        OSL_TRACE("enable unknown control %d", controlId );
}

void SAL_CALL KDE5FilePicker::setLabel( sal_Int16 controlId, const OUString &label )
    throw( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT setLabelSignal( controlId, label );
    }

    if (_customWidgets.contains( controlId )) {
        QCheckBox* cb = dynamic_cast<QCheckBox*>( _customWidgets.value( controlId ));
        if (cb)
            cb->setText( toQString(label) );
    }
    else
        OSL_TRACE( "set label on unknown control %d", controlId );
}

OUString SAL_CALL KDE5FilePicker::getLabel(sal_Int16 controlId)
    throw ( uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT getLabelSignal( controlId );
    }

    QString label;
    if (_customWidgets.contains( controlId )) {
        QCheckBox* cb = dynamic_cast<QCheckBox*>( _customWidgets.value( controlId ));
        if (cb)
            label = cb->text();
    }
    else
        OSL_TRACE( "get label on unknown control %d", controlId );

    return toOUString(label);
}

QString KDE5FilePicker::getResString( sal_Int16 aRedId )
{
    QString aResString;

    if( aRedId < 0 )
        return aResString;

    try
    {
        aResString = toQString(ResId(aRedId, *ImplGetResMgr()).toString());
    }
    catch(...)
    {
    }

    return aResString.replace('~', '&');
}

void KDE5FilePicker::addCustomControl(sal_Int16 controlId)
{
    QWidget* widget = nullptr;
    sal_Int32 resId = -1;

    switch (controlId)
    {
        case CHECKBOX_AUTOEXTENSION:
            resId = STR_FPICKER_AUTO_EXTENSION;
            break;
        case CHECKBOX_PASSWORD:
            resId = STR_FPICKER_PASSWORD;
            break;
        case CHECKBOX_FILTEROPTIONS:
            resId = STR_FPICKER_FILTER_OPTIONS;
            break;
        case CHECKBOX_READONLY:
            resId = STR_FPICKER_READONLY;
            break;
        case CHECKBOX_LINK:
            resId = STR_FPICKER_INSERT_AS_LINK;
            break;
        case CHECKBOX_PREVIEW:
            resId = STR_FPICKER_SHOW_PREVIEW;
            break;
        case CHECKBOX_SELECTION:
            resId = STR_FPICKER_SELECTION;
            break;
        case PUSHBUTTON_PLAY:
            resId = STR_FPICKER_PLAY;
            break;
        case LISTBOX_VERSION:
            resId = STR_FPICKER_VERSION;
            break;
        case LISTBOX_TEMPLATE:
            resId = STR_FPICKER_TEMPLATES;
            break;
        case LISTBOX_IMAGE_TEMPLATE:
            resId = STR_FPICKER_IMAGE_TEMPLATE;
            break;
        case LISTBOX_VERSION_LABEL:
        case LISTBOX_TEMPLATE_LABEL:
        case LISTBOX_IMAGE_TEMPLATE_LABEL:
        case LISTBOX_FILTER_SELECTOR:
            break;
    }

    switch (controlId)
    {
        case CHECKBOX_AUTOEXTENSION:
        case CHECKBOX_PASSWORD:
        case CHECKBOX_FILTEROPTIONS:
        case CHECKBOX_READONLY:
        case CHECKBOX_LINK:
        case CHECKBOX_PREVIEW:
        case CHECKBOX_SELECTION:
        {
            widget = new QCheckBox(getResString(resId), _extraControls);

            // the checkbox is created even for CHECKBOX_AUTOEXTENSION to simplify
            // code, but the checkbox is hidden and ignored
            if( controlId == CHECKBOX_AUTOEXTENSION )
                widget->hide();

            break;
        }
        case PUSHBUTTON_PLAY:
        case LISTBOX_VERSION:
        case LISTBOX_TEMPLATE:
        case LISTBOX_IMAGE_TEMPLATE:
        case LISTBOX_VERSION_LABEL:
        case LISTBOX_TEMPLATE_LABEL:
        case LISTBOX_IMAGE_TEMPLATE_LABEL:
        case LISTBOX_FILTER_SELECTOR:
            break;
    }

    if (widget)
    {
        _layout->addWidget(widget);
        _customWidgets.insert(controlId, widget);
    }
}

void SAL_CALL KDE5FilePicker::initialize( const uno::Sequence<uno::Any> &args )
    throw( uno::Exception, uno::RuntimeException, std::exception )
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT initializeSignal( args );
    }

    _filters.clear();
    _currentFilter.clear();

    // parameter checking
    uno::Any arg;
    if (args.getLength() == 0)
    {
        throw lang::IllegalArgumentException(
                OUString( "no arguments" ),
                static_cast< XFilePicker2* >( this ), 1 );
    }

    arg = args[0];

    if (( arg.getValueType() != cppu::UnoType<sal_Int16>::get()) &&
        ( arg.getValueType() != cppu::UnoType<sal_Int8>::get()))
    {
        throw lang::IllegalArgumentException(
                OUString( "invalid argument type" ),
                static_cast< XFilePicker2* >( this ), 1 );
    }

    sal_Int16 templateId = -1;
    arg >>= templateId;

    //default is opening
    QFileDialog::AcceptMode operationMode = QFileDialog::AcceptOpen;

    switch ( templateId )
    {
        case FILEOPEN_SIMPLE:
            break;

        case FILESAVE_SIMPLE:
            operationMode = QFileDialog::AcceptSave;
            break;

        case FILESAVE_AUTOEXTENSION:
            operationMode = QFileDialog::AcceptSave;
            addCustomControl( CHECKBOX_AUTOEXTENSION );
            break;

        case FILESAVE_AUTOEXTENSION_PASSWORD:
        {
            operationMode = QFileDialog::AcceptSave;
            addCustomControl( CHECKBOX_PASSWORD );
            break;
        }
        case FILESAVE_AUTOEXTENSION_PASSWORD_FILTEROPTIONS:
        {
            operationMode = QFileDialog::AcceptSave;
            addCustomControl( CHECKBOX_AUTOEXTENSION );
            addCustomControl( CHECKBOX_PASSWORD );
            addCustomControl( CHECKBOX_FILTEROPTIONS );
            break;
        }
        case FILESAVE_AUTOEXTENSION_SELECTION:
            operationMode = QFileDialog::AcceptSave;
            addCustomControl( CHECKBOX_AUTOEXTENSION );
            addCustomControl( CHECKBOX_SELECTION );
            break;

        case FILESAVE_AUTOEXTENSION_TEMPLATE:
            operationMode = QFileDialog::AcceptSave;
            addCustomControl( CHECKBOX_AUTOEXTENSION );
            addCustomControl( LISTBOX_TEMPLATE );
            break;

        case FILEOPEN_LINK_PREVIEW_IMAGE_TEMPLATE:
            addCustomControl( CHECKBOX_LINK );
            addCustomControl( CHECKBOX_PREVIEW );
            addCustomControl( LISTBOX_IMAGE_TEMPLATE );
            break;

        case FILEOPEN_PLAY:
            addCustomControl( PUSHBUTTON_PLAY );
            break;

        case FILEOPEN_READONLY_VERSION:
            addCustomControl( CHECKBOX_READONLY );
            addCustomControl( LISTBOX_VERSION );
            break;

        case FILEOPEN_LINK_PREVIEW:
            addCustomControl( CHECKBOX_LINK );
            addCustomControl( CHECKBOX_PREVIEW );
            break;

        default:
            throw lang::IllegalArgumentException(
                    OUString( "Unknown template" ),
                    static_cast< XFilePicker2* >( this ),
                    1 );
    }

    _dialog->setAcceptMode( operationMode );

    sal_Int16 resId = -1;
    switch (_dialog->acceptMode())
    {
    case QFileDialog::AcceptOpen:
        resId = STR_FPICKER_OPEN;
        break;
    case QFileDialog::AcceptSave:
        resId = STR_FPICKER_SAVE;
        _dialog->setConfirmOverwrite( true );
        break;
    default:
        break;
    }

    _dialog->setWindowTitle(getResString(resId));
}

void SAL_CALL KDE5FilePicker::cancel()
    throw ( uno::RuntimeException, std::exception )
{

}

void SAL_CALL KDE5FilePicker::disposing( const lang::EventObject &rEvent )
    throw( uno::RuntimeException )
{
    uno::Reference<XFilePickerListener> xFilePickerListener( rEvent.Source, uno::UNO_QUERY );

    if ( xFilePickerListener.is() )
    {
        removeFilePickerListener( xFilePickerListener );
    }
}

OUString SAL_CALL KDE5FilePicker::getImplementationName()
    throw( uno::RuntimeException, std::exception )
{
    return OUString( FILE_PICKER_IMPL_NAME );
}

sal_Bool SAL_CALL KDE5FilePicker::supportsService( const OUString& ServiceName )
    throw( uno::RuntimeException, std::exception )
{
    return cppu::supportsService(this, ServiceName);
}

uno::Sequence< OUString > SAL_CALL KDE5FilePicker::getSupportedServiceNames()
    throw( uno::RuntimeException, std::exception )
{
    return FilePicker_getSupportedServiceNames();
}

void KDE5FilePicker::checkProtocol()
{
    if( qApp->thread() != QThread::currentThread() ) {
        SalYieldMutexReleaser release;
        return Q_EMIT checkProtocolSignal();
    }

    // There's no libreoffice.desktop :(, so find a matching one.
/*
    KService::List services = KServiceTypeTrader::self()->query( "Application", "Exec =~ 'libreoffice %U'" );
    QStringList protocols;
    if( !services.isEmpty())
        protocols = services[ 0 ]->property( "X-KDE-Protocols" ).toStringList();
    if( protocols.isEmpty()) // incorrect (developer?) installation ?
        protocols << "file" << "http";
    if( !protocols.contains( _dialog->baseUrl().protocol()) && !protocols.contains( "KIO" ))
        KMessageBox::error( _dialog, KIO::buildErrorString( KIO::ERR_UNSUPPORTED_PROTOCOL, _dialog->baseUrl().protocol()));
*/
}

void KDE5FilePicker::filterChanged(const QString &)
{
    FilePickerEvent aEvent;
    aEvent.ElementId = LISTBOX_FILTER;
    OSL_TRACE( "filter changed" );
    if (m_xListener.is())
        m_xListener->controlStateChanged( aEvent );
}

void KDE5FilePicker::selectionChanged()
{
    FilePickerEvent aEvent;
    OSL_TRACE( "file selection changed" );
    if (m_xListener.is())
        m_xListener->fileSelectionChanged( aEvent );
}

#include "KDE5FilePicker.moc"

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */

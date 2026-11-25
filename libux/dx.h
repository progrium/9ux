/*
 * The drawing layer's names. libux's controls and windows are drawn
 * by functions that once were the API; the API (tapi.c) now makes
 * uitree nodes and draws through these. Files that implement or draw
 * with them include this before ux.h.
 */
#define uxinit	dxinit
#define uxwindow	dxwindow
#define uxdialog	dxdialog
#define uxwinhandler	dxwinhandler
#define uxwinsettitle	dxwinsettitle
#define uxwinclose	dxwinclose
#define uxbutton	dxbutton
#define uxtextfield	dxtextfield
#define uxcheckbox	dxcheckbox
#define uxlabel	dxlabel
#define uxgroup	dxgroup
#define uxlist	dxlist
#define uxiconview	dxiconview
#define uxscrollview	dxscrollview
#define uxfilechooser	dxfilechooser
#define uxconsole	dxconsole
#define uxdelete	dxdelete
#define uxhandler	dxhandler
#define uxsetparent	dxsetparent
#define uxsetrect	dxsetrect
#define uxsetvalue	dxsetvalue
#define uxsetlabel	dxsetlabel
#define uxenable	dxenable
#define uxshow	dxshow
#define uxsetitems	dxsetitems
#define uxsetcolumns	dxsetcolumns
#define uxiconadd	dxiconadd
#define uxicondel	dxicondel
#define uxiconclear	dxiconclear
#define uxiconset	dxiconset
#define uxiconselect	dxiconselect
#define uxconswrite	dxconswrite
#define uxconsclear	dxconsclear
#define uxconsraw	dxconsraw
#define uxmenu	dxmenu
#define uxitem	dxitem
#define uxsep	dxsep
#define uxitemenable	dxitemenable
#define uxitemcheck	dxitemcheck
#define uxsetorigin	dxsetorigin

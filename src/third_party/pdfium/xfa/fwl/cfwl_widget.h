// Copyright 2014 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef XFA_FWL_CFWL_WIDGET_H_
#define XFA_FWL_CFWL_WIDGET_H_

#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/unowned_ptr.h"
#include "fxjs/gc/heap.h"
#include "v8/include/cppgc/garbage-collected.h"
#include "v8/include/cppgc/macros.h"
#include "v8/include/cppgc/member.h"
#include "v8/include/cppgc/prefinalizer.h"
#include "xfa/fde/cfde_data.h"
#include "xfa/fwl/cfwl_themepart.h"
#include "xfa/fwl/cfwl_widgetmgr.h"
#include "xfa/fwl/fwl_widgetdef.h"
#include "xfa/fwl/fwl_widgethit.h"
#include "xfa/fwl/ifwl_widgetdelegate.h"

class CFWL_App;
class CFWL_Event;
class CFWL_Widget;
class IFWL_ThemeProvider;

enum class FWL_Type {
  Unknown = 0,

  Barcode,
  Caret,
  CheckBox,
  ComboBox,
  DateTimePicker,
  Edit,
  Form,
  FormProxy,
  ListBox,
  MonthCalendar,
  PictureBox,
  PushButton,
  ScrollBar,
  SpinButton,
  ToolTip
};

// NOTE: CFWL_Widget serves as its own delegate until replaced at runtime.
class CFWL_Widget : public cppgc::GarbageCollected<CFWL_Widget>,
                    public IFWL_WidgetDelegate {
  CPPGC_USING_PRE_FINALIZER(CFWL_Widget, PreFinalize);

 public:
  class AdapterIface {
   public:
    virtual ~AdapterIface() = default;
    virtual CFX_Matrix GetRotateMatrix() = 0;
    virtual void DisplayCaret(bool bVisible, const CFX_RectF* pRtAnchor) = 0;
    virtual void GetBorderColorAndThickness(FX_ARGB* cr, float* fWidth) = 0;
  };

  class Properties {
   public:
    uint32_t m_dwStyles = FWL_WGTSTYLE_Child;
    uint32_t m_dwStyleExes = 0;
    uint32_t m_dwStates = 0;
  };

  class ScopedUpdateLock {
    CPPGC_STACK_ALLOCATED();  // Allow raw/unowned pointers.

   public:
    explicit ScopedUpdateLock(CFWL_Widget* widget);
    ~ScopedUpdateLock();

   private:
    UnownedPtr<CFWL_Widget> const widget_;  // Ok, stack-only.
  };

  CONSTRUCT_VIA_MAKE_GARBAGE_COLLECTED;
  ~CFWL_Widget() override;

  virtual void PreFinalize();
  void Trace(cppgc::Visitor* visitor) const override;

  virtual FWL_Type GetClassID() const = 0;
  virtual bool IsForm() const;
  virtual CFX_RectF GetAutosizedWidgetRect();
  virtual CFX_RectF GetWidgetRect();
  virtual CFX_RectF GetClientRect();
  virtual void ModifyStylesEx(uint32_t dwStylesExAdded,
                              uint32_t dwStylesExRemoved);
  virtual void SetStates(uint32_t dwStates);
  virtual void RemoveStates(uint32_t dwStates);
  virtual void Update() = 0;
  virtual FWL_WidgetHit HitTest(const CFX_PointF& point);
  virtual void DrawWidget(CFGAS_GEGraphics* pGraphics,
                          const CFX_Matrix& matrix) = 0;

  // IFWL_WidgetDelegate:
  void OnProcessMessage(CFWL_Message* pMessage) override;
  void OnProcessEvent(CFWL_Event* pEvent) override;
  void OnDrawWidget(CFGAS_GEGraphics* pGraphics,
                    const CFX_Matrix& matrix) override;

  void InflateWidgetRect(CFX_RectF& rect);
  void SetWidgetRect(const CFX_RectF& rect);

  bool IsVisible() const;
  bool IsOverLapper() const;
  bool IsPopup() const;
  bool IsChild() const;

  CFWL_WidgetMgr* GetWidgetMgr() const { return m_pWidgetMgr; }
  CFWL_Widget* GetOuter() const { return m_pOuter; }
  CFWL_Widget* GetOutmost() const;

  void ModifyStyles(uint32_t dwStylesAdded, uint32_t dwStylesRemoved);
  uint32_t GetStylesEx() const { return m_Properties.m_dwStyleExes; }
  uint32_t GetStates() const { return m_Properties.m_dwStates; }

  CFX_PointF TransformTo(CFWL_Widget* pWidget, const CFX_PointF& point);
  CFX_Matrix GetMatrix() const;
  IFWL_ThemeProvider* GetThemeProvider() const;
  void SetDelegate(IFWL_WidgetDelegate* delegate) { m_pDelegate = delegate; }
  IFWL_WidgetDelegate* GetDelegate() {
    return m_pDelegate ? m_pDelegate.Get() : this;
  }
  const IFWL_WidgetDelegate* GetDelegate() const {
    return m_pDelegate ? m_pDelegate.Get() : this;
  }

  CFWL_App* GetFWLApp() const { return m_pFWLApp.Get(); }
  uint64_t GetEventKey() const { return m_nEventKey; }
  void SetEventKey(uint64_t key) { m_nEventKey = key; }

  AdapterIface* GetAdapterIface() const { return m_pAdapterIface; }
  void SetAdapterIface(AdapterIface* pItem) { m_pAdapterIface = pItem; }
  void RepaintRect(const CFX_RectF& pRect);

 protected:
  CFWL_Widget(CFWL_App* app, const Properties& properties, CFWL_Widget* pOuter);

  bool IsEnabled() const;
  bool IsLocked() const { return m_iLock > 0; }
  bool HasBorder() const;
  CFX_RectF GetEdgeRect() const;
  float GetCXBorderSize() const;
  float GetCYBorderSize() const;
  CFX_RectF GetRelativeRect() const;
  CFX_SizeF CalcTextSize(const WideString& wsText,
                         bool bMultiLine);
  void CalcTextRect(const WideString& wsText,
                    const FDE_TextStyle& dwTTOStyles,
                    FDE_TextAlignment iTTOAlign,
                    CFX_RectF* pRect);
  void SetGrab(bool bSet);
  void RegisterEventTarget(CFWL_Widget* pEventSource);
  void UnregisterEventTarget();
  void DispatchEvent(CFWL_Event* pEvent);
  void DrawBorder(CFGAS_GEGraphics* pGraphics,
                  CFWL_Part iPartBorder,
                  const CFX_Matrix& pMatrix);

  Properties m_Properties;
  CFX_RectF m_WidgetRect;

 private:
  void LockUpdate() { m_iLock++; }
  void UnlockUpdate() {
    if (IsLocked())
      m_iLock--;
  }

  CFWL_Widget* GetParent() const { return m_pWidgetMgr->GetParentWidget(this); }
  CFX_SizeF GetOffsetFromParent(CFWL_Widget* pParent);
  void DrawBackground(CFGAS_GEGraphics* pGraphics,
                      CFWL_Part iPartBk,
                      const CFX_Matrix* pMatrix);
  void NotifyDriver();
  bool IsParent(CFWL_Widget* pParent);

  int32_t m_iLock = 0;
  uint64_t m_nEventKey = 0;
  AdapterIface* m_pAdapterIface = nullptr;
  cppgc::Member<CFWL_App> const m_pFWLApp;
  cppgc::Member<CFWL_WidgetMgr> const m_pWidgetMgr;
  cppgc::Member<IFWL_WidgetDelegate> m_pDelegate;
  cppgc::Member<CFWL_Widget> const m_pOuter;
};

#endif  // XFA_FWL_CFWL_WIDGET_H_

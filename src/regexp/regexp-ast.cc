// Copyright 2016 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ostreams.h"
#include "src/regexp/regexp-ast.h"

namespace v8 {
namespace internal {

#define MAKE_ACCEPT(Name)                                          \
  void* RegExp##Name::Accept(RegExpVisitor* visitor, void* data) { \
    return visitor->Visit##Name(this, data);                       \
  }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_ACCEPT)
#undef MAKE_ACCEPT

#define MAKE_TYPE_CASE(Name)                            \
  RegExp##Name* RegExpTree::As##Name() { return NULL; } \
  bool RegExpTree::Is##Name() { return false; }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_TYPE_CASE)
#undef MAKE_TYPE_CASE

#define MAKE_TYPE_CASE(Name)                              \
  RegExp##Name* RegExp##Name::As##Name() { return this; } \
  bool RegExp##Name::Is##Name() { return true; }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_TYPE_CASE)
#undef MAKE_TYPE_CASE


static Interval ListCaptureRegisters(ZoneList<RegExpTree*>* children) {
  Interval result = Interval::Empty();
  for (int i = 0; i < children->length(); i++)
    result = result.Union(children->at(i)->CaptureRegisters());
  return result;
}


Interval RegExpAlternative::CaptureRegisters() {
  return ListCaptureRegisters(nodes());
}


Interval RegExpDisjunction::CaptureRegisters() {
  return ListCaptureRegisters(alternatives());
}


Interval RegExpLookaround::CaptureRegisters() {
  return body()->CaptureRegisters();
}


Interval RegExpCapture::CaptureRegisters() {
  Interval self(StartRegister(index()), EndRegister(index()));
  return self.Union(body()->CaptureRegisters());
}


Interval RegExpQuantifier::CaptureRegisters() {
  return body()->CaptureRegisters();
}


bool RegExpAssertion::IsAnchoredAtStart() {
  return assertion_type() == RegExpAssertion::START_OF_INPUT;
}


bool RegExpAssertion::IsAnchoredAtEnd() {
  return assertion_type() == RegExpAssertion::END_OF_INPUT;
}


bool RegExpAlternative::IsAnchoredAtStart() {
  ZoneList<RegExpTree*>* nodes = this->nodes();
  for (int i = 0; i < nodes->length(); i++) {
    RegExpTree* node = nodes->at(i);
    if (node->IsAnchoredAtStart()) {
      return true;
    }
    if (node->max_match() > 0) {
      return false;
    }
  }
  return false;
}


bool RegExpAlternative::IsAnchoredAtEnd() {
  ZoneList<RegExpTree*>* nodes = this->nodes();
  for (int i = nodes->length() - 1; i >= 0; i--) {
    RegExpTree* node = nodes->at(i);
    if (node->IsAnchoredAtEnd()) {
      return true;
    }
    if (node->max_match() > 0) {
      return false;
    }
  }
  return false;
}


bool RegExpDisjunction::IsAnchoredAtStart() {
  ZoneList<RegExpTree*>* alternatives = this->alternatives();
  for (int i = 0; i < alternatives->length(); i++) {
    if (!alternatives->at(i)->IsAnchoredAtStart()) return false;
  }
  return true;
}


bool RegExpDisjunction::IsAnchoredAtEnd() {
  ZoneList<RegExpTree*>* alternatives = this->alternatives();
  for (int i = 0; i < alternatives->length(); i++) {
    if (!alternatives->at(i)->IsAnchoredAtEnd()) return false;
  }
  return true;
}


bool RegExpLookaround::IsAnchoredAtStart() {
  return is_positive() && type() == LOOKAHEAD && body()->IsAnchoredAtStart();
}


bool RegExpCapture::IsAnchoredAtStart() { return body()->IsAnchoredAtStart(); }


bool RegExpCapture::IsAnchoredAtEnd() { return body()->IsAnchoredAtEnd(); }


// Convert regular expression trees to a simple sexp representation.
// This representation should be different from the input grammar
// in as many cases as possible, to make it more difficult for incorrect
// parses to look as correct ones which is likely if the input and
// output formats are alike.
class RegExpUnparser final : public RegExpVisitor {
 public:
  RegExpUnparser(v8::base::OStream& os, Zone* zone) : os_(os), zone_(zone) {}
  void VisitCharacterRange(CharacterRange that);
#define MAKE_CASE(Name) void* Visit##Name(RegExp##Name*, void* data) override;
  FOR_EACH_REG_EXP_TREE_TYPE(MAKE_CASE)
#undef MAKE_CASE
 private:
  v8::base::OStream& os_;
  Zone* zone_;
};


void* RegExpUnparser::VisitDisjunction(RegExpDisjunction* that, void* data) {
  os_ << u8"(|";
  for (int i = 0; i < that->alternatives()->length(); i++) {
    os_ << u8" ";
    that->alternatives()->at(i)->Accept(this, data);
  }
  os_ << u8")";
  return NULL;
}


void* RegExpUnparser::VisitAlternative(RegExpAlternative* that, void* data) {
  os_ << u8"(:";
  for (int i = 0; i < that->nodes()->length(); i++) {
    os_ << u8" ";
    that->nodes()->at(i)->Accept(this, data);
  }
  os_ << u8")";
  return NULL;
}


void RegExpUnparser::VisitCharacterRange(CharacterRange that) {
  os_ << AsUC32(that.from());
  if (!that.IsSingleton()) {
    os_ << u8"-" << AsUC32(that.to());
  }
}


void* RegExpUnparser::VisitCharacterClass(RegExpCharacterClass* that,
                                          void* data) {
  if (that->is_negated()) os_ << u8"^";
  os_ << u8"[";
  for (int i = 0; i < that->ranges(zone_)->length(); i++) {
    if (i > 0) os_ << u8" ";
    VisitCharacterRange(that->ranges(zone_)->at(i));
  }
  os_ << u8"]";
  return NULL;
}


void* RegExpUnparser::VisitAssertion(RegExpAssertion* that, void* data) {
  switch (that->assertion_type()) {
    case RegExpAssertion::START_OF_INPUT:
      os_ << u8"@^i";
      break;
    case RegExpAssertion::END_OF_INPUT:
      os_ << u8"@$i";
      break;
    case RegExpAssertion::START_OF_LINE:
      os_ << u8"@^l";
      break;
    case RegExpAssertion::END_OF_LINE:
      os_ << u8"@$l";
      break;
    case RegExpAssertion::BOUNDARY:
      os_ << u8"@b";
      break;
    case RegExpAssertion::NON_BOUNDARY:
      os_ << u8"@B";
      break;
  }
  return NULL;
}


void* RegExpUnparser::VisitAtom(RegExpAtom* that, void* data) {
  os_ << u8"'";
  Vector<const uc16> chardata = that->data();
  for (int i = 0; i < chardata.length(); i++) {
    os_ << AsUC16(chardata[i]);
  }
  os_ << u8"'";
  return NULL;
}


void* RegExpUnparser::VisitText(RegExpText* that, void* data) {
  if (that->elements()->length() == 1) {
    that->elements()->at(0).tree()->Accept(this, data);
  } else {
    os_ << u8"(!";
    for (int i = 0; i < that->elements()->length(); i++) {
      os_ << u8" ";
      that->elements()->at(i).tree()->Accept(this, data);
    }
    os_ << u8")";
  }
  return NULL;
}


void* RegExpUnparser::VisitQuantifier(RegExpQuantifier* that, void* data) {
  os_ << u8"(# " << that->min() << u8" ";
  if (that->max() == RegExpTree::kInfinity) {
    os_ << u8"- ";
  } else {
    os_ << that->max() << u8" ";
  }
  os_ << (that->is_greedy() ? u8"g " : that->is_possessive() ? u8"p " : u8"n ");
  that->body()->Accept(this, data);
  os_ << u8")";
  return NULL;
}


void* RegExpUnparser::VisitCapture(RegExpCapture* that, void* data) {
  os_ << u8"(^ ";
  that->body()->Accept(this, data);
  os_ << u8")";
  return NULL;
}


void* RegExpUnparser::VisitLookaround(RegExpLookaround* that, void* data) {
  os_ << u8"(";
  os_ << (that->type() == RegExpLookaround::LOOKAHEAD ? u8"->" : u8"<-");
  os_ << (that->is_positive() ? u8" + " : u8" - ");
  that->body()->Accept(this, data);
  os_ << u8")";
  return NULL;
}


void* RegExpUnparser::VisitBackReference(RegExpBackReference* that,
                                         void* data) {
  os_ << u8"(<- " << that->index() << u8")";
  return NULL;
}


void* RegExpUnparser::VisitEmpty(RegExpEmpty* that, void* data) {
  os_ << '\x25';
  return NULL;
}


v8::base::OStream& RegExpTree::Print(v8::base::OStream& os, Zone* zone) {  // NOLINT
  RegExpUnparser unparser(os, zone);
  Accept(&unparser, NULL);
  return os;
}


RegExpDisjunction::RegExpDisjunction(ZoneList<RegExpTree*>* alternatives)
    : alternatives_(alternatives) {
  DCHECK(alternatives->length() > 1);
  RegExpTree* first_alternative = alternatives->at(0);
  min_match_ = first_alternative->min_match();
  max_match_ = first_alternative->max_match();
  for (int i = 1; i < alternatives->length(); i++) {
    RegExpTree* alternative = alternatives->at(i);
    min_match_ = Min(min_match_, alternative->min_match());
    max_match_ = Max(max_match_, alternative->max_match());
  }
}


static int IncreaseBy(int previous, int increase) {
  if (RegExpTree::kInfinity - previous < increase) {
    return RegExpTree::kInfinity;
  } else {
    return previous + increase;
  }
}


RegExpAlternative::RegExpAlternative(ZoneList<RegExpTree*>* nodes)
    : nodes_(nodes) {
  DCHECK(nodes->length() > 1);
  min_match_ = 0;
  max_match_ = 0;
  for (int i = 0; i < nodes->length(); i++) {
    RegExpTree* node = nodes->at(i);
    int node_min_match = node->min_match();
    min_match_ = IncreaseBy(min_match_, node_min_match);
    int node_max_match = node->max_match();
    max_match_ = IncreaseBy(max_match_, node_max_match);
  }
}


}  // namespace internal
}  // namespace v8

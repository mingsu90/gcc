/* Code for range operators.
   Copyright (C) 2017-2019 Free Software Foundation, Inc.
   Contributed by Andrew MacLeod <amacleod@redhat.com>
   and Aldy Hernandez <aldyh@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "insn-codes.h"
#include "rtl.h"
#include "tree.h"
#include "gimple.h"
#include "cfghooks.h"
#include "tree-pass.h"
#include "ssa.h"
#include "optabs-tree.h"
#include "gimple-pretty-print.h"
#include "diagnostic-core.h"
#include "flags.h"
#include "fold-const.h"
#include "stor-layout.h"
#include "calls.h"
#include "cfganal.h"
#include "gimple-fold.h"
#include "tree-eh.h"
#include "gimple-iterator.h"
#include "gimple-walk.h"
#include "tree-cfg.h"
#include "wide-int.h"
#include "range-op.h"
#include "wide-int-range.h"

// Auxiliary routine to return the upper limit for a type.

inline wide_int
max_limit (const_tree type)
{
  return wi::max_value (TYPE_PRECISION (type) , TYPE_SIGN (type));
}

// Auxiliary routine to return the lower limit for a type.

inline wide_int
min_limit (const_tree type)
{
  return wi::min_value (TYPE_PRECISION (type) , TYPE_SIGN (type));
}


// Default wide_int fold operation returns [min , max].
irange
range_operator::wi_fold (tree type,
			 const wide_int &lh_lb ATTRIBUTE_UNUSED,
			 const wide_int &lh_ub ATTRIBUTE_UNUSED,
			 const wide_int &rh_lb ATTRIBUTE_UNUSED,
			 const wide_int &rh_ub ATTRIBUTE_UNUSED) const
{
  return irange (type);
}

// The default for fold is to break all ranges into subranges
// and invoke the 'wi_fold' method on each subrange pair.
irange
range_operator::fold_range (tree type, const irange &lh,
			    const irange &rh) const
{
  irange r;
  if (lh.undefined_p () || rh.undefined_p ())
    return r;

  for (unsigned x = 0; x < lh.num_pairs (); ++x)
    for (unsigned y = 0; y < rh.num_pairs (); ++y)
      {
	wide_int lh_lb = lh.lower_bound (x);
	wide_int lh_ub = lh.upper_bound (x);
	wide_int rh_lb = rh.lower_bound (y);
	wide_int rh_ub = rh.upper_bound (y);
	r.union_ (wi_fold (type, lh_lb, lh_ub, rh_lb, rh_ub));
	if (r.varying_p ())
	  return r;
      }

  return r;
}

// The default for op1_range is to return false.
bool
range_operator::op1_range (irange &r ATTRIBUTE_UNUSED,
			   tree type ATTRIBUTE_UNUSED,
			   const irange &lhs ATTRIBUTE_UNUSED,
			   const irange &op2 ATTRIBUTE_UNUSED) const
{
  return false;
}

// The default for op2_range is to return false.
bool
range_operator::op2_range (irange &r ATTRIBUTE_UNUSED,
			   tree type ATTRIBUTE_UNUSED,
			   const irange &lhs ATTRIBUTE_UNUSED,
			   const irange &op1 ATTRIBUTE_UNUSED) const
{
  return false;
}

// -------------------------------------------------------------------------
// -------------------------------------------------------------------------

// If the range of either op1 or op2 is undefined, set the result to
// undefined and return true.  

inline bool
empty_range_check (irange &r, const irange &op1, const irange & op2)
{
  if (op1.undefined_p () || op2.undefined_p ())
    {
      r.set_undefined ();
      return true;
    }
  else
    return false;
}

// Called when there is either an overflow OR an underflow... which means 
// an anti range must be created to compensate.   This does not cover
// the case where there are 2 possible overflows, or none.

static void
adjust_overflow_bound (irange &r, tree type, const wide_int &wmin,
		       const wide_int &wmax)
{
  const signop sgn = TYPE_SIGN (type);
  const unsigned int prec = TYPE_PRECISION (type);

  wide_int tmin = wide_int::from (wmin, prec, sgn);
  wide_int tmax = wide_int::from (wmax, prec, sgn);

  bool covers = false;
  wide_int tem = tmin;
  tmin = tmax + 1;
  if (wi::cmp (tmin, tmax, sgn) < 0)
    covers = true;
  tmax = tem - 1;
  if (wi::cmp (tmax, tem, sgn) > 0)
    covers = true;
  /* If the anti-range would cover nothing, drop to varying.
     Likewise if the anti-range bounds are outside of the
     types values.  */
  if (covers || wi::cmp (tmin, tmax, sgn) > 0)
    r.set_varying (type);
  else
    {
      irange tmp (VR_ANTI_RANGE, type, tmin, tmax);
      r.union_ (tmp);
    }
  return;
}

// Given newly calculated lbound and ubound, examine their respective
// overflow bits to determine how to add [lbound, ubound] into range R.

static void
accumulate_range (irange &r, tree type, const wide_int &wmin,
		  const wide_int &wmax,
		  wi::overflow_type min_ovf = wi::OVF_NONE,
		  wi::overflow_type max_ovf = wi::OVF_NONE)
{
  const signop sgn = TYPE_SIGN (type);
  const unsigned int prec = TYPE_PRECISION (type);
  const bool overflow_wraps = TYPE_OVERFLOW_WRAPS (type);

  // For one bit precision if max != min, then the range covers all values.  
  if (prec == 1 && wi::ne_p (wmax, wmin))
    {
      r.set_varying (type);
      return;
    }

  if (overflow_wraps)
    {
      /* If overflow wraps, truncate the values and adjust the
	 range kind and bounds appropriately.  */
      if ((min_ovf != wi::OVF_NONE) == (max_ovf != wi::OVF_NONE))
	{
	  wide_int tmin = wide_int::from (wmin, prec, sgn);
	  wide_int tmax = wide_int::from (wmax, prec, sgn);
	  /* If the limits are swapped, we wrapped around and cover
	     the entire range.  We have a similar check at the end of
	     extract_range_from_binary_expr.  */
	  if (wi::gt_p (tmin, tmax, sgn))
	    r.set_varying (type);
	  else
	    {
	      /* No overflow or both overflow or underflow.  The
		 range kind stays normal.  */
	      irange tmp (type, tmin, tmax);
	      r.union_ (tmp);
	    }
	}
      else if ((min_ovf == wi::OVF_UNDERFLOW && max_ovf == wi::OVF_NONE)
	       || (max_ovf == wi::OVF_OVERFLOW && min_ovf == wi::OVF_NONE))
	adjust_overflow_bound (r, type, wmin, wmax);
      else
	// Other underflow and/or overflow, drop to VR_VARYING. 
	r.set_varying (type);
    }
  else
    {
      /* If overflow does not wrap, saturate to the types min/max
	 value.  */
      wide_int new_lb, new_ub;
      if (min_ovf == wi::OVF_UNDERFLOW)
	new_lb = wi::min_value (prec, sgn);
      else if (min_ovf == wi::OVF_OVERFLOW)
	new_lb = wi::max_value (prec, sgn);
      else
        new_lb = wmin;

      if (max_ovf == wi::OVF_UNDERFLOW)
	new_ub = wi::min_value (prec, sgn);
      else if (max_ovf == wi::OVF_OVERFLOW)
	new_ub = wi::max_value (prec, sgn);
      else
        new_ub = wmax;
      irange tmp (type, new_lb, new_ub);
      r.union_ (tmp);
    }
}

/* Like accumulate_range, but canonicalize the case where the bounds
   are swapped and overflow may wrap.  In which case we transform
   [10,5] into [MIN,5][10,MAX].  */

static inline void
accumulate_possibly_reversed_range (irange &r, tree type,
				    const wide_int &new_lb,
				    const wide_int &new_ub)
{
  signop s = TYPE_SIGN (type);
  // If the bounds are swapped, treat the result as if an overflow occured.
  if (wi::gt_p (new_lb, new_ub, s))
    {
      adjust_overflow_bound (r, type, new_lb, new_ub);
      return;
    }
  // Otherwise its just a normal range.
  irange tmp (type, new_lb, new_ub);
  r.union_ (tmp);
}

// Return an irange instance that is a boolean TRUE.

static irange
range_true (tree type)
{
  unsigned prec = TYPE_PRECISION (type);
  return irange (type, wi::one (prec), wi::one (prec));
}

// Return an irange instance that is a boolean FALSE.

static irange
range_false (tree type)
{
  unsigned prec = TYPE_PRECISION (type);
  return irange (type, wi::zero (prec), wi::zero (prec));
}

// Return an irange instance that is a boolean FALSE.

static irange
range_true_and_false (tree type)
{
  unsigned prec = TYPE_PRECISION (type);
  return irange (type, wi::zero (prec), wi::one (prec));
}

enum bool_range_state { BRS_FALSE, BRS_TRUE, BRS_EMPTY, BRS_FULL };

/* Return the summary information about boolean range LHS.  Return an
   "interesting" range in R.  for EMPTY or FULL, return the equivilent
   range for TYPE, for BRS_TRUE and BRS false, return the negatiuon of
   the bool range.  */
static bool_range_state
get_bool_state (irange &r, const irange &lhs, tree val_type)
{
  /* If there is no result, then this is unexectuable, so no range. */
  if (lhs.undefined_p ())
    {
      r.set_undefined ();
      return BRS_EMPTY;
    }

  // If the bounds arent the same, then its not a constant.  */
  if (!wi::eq_p (lhs.upper_bound (), lhs.lower_bound ()))
    {
      r.set_varying (val_type);
      return BRS_FULL;
    }

  if (lhs.zero_p ())
    return BRS_FALSE;

  return BRS_TRUE;
}


class operator_equal : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			  const irange &val) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			  const irange &val) const;
} op_equal;



/* Fold comparison of the 2 ranges.  */
irange
operator_equal::fold_range (tree type,
			    const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  /* We can be sure the values are always equal or not if both ranges
     consist of a single value, and then compare them.  */
  if (wi::eq_p (op1.lower_bound (), op1.upper_bound ())
      && wi::eq_p (op2.lower_bound (), op2.upper_bound ()))
    {
      if (wi::eq_p (op1.lower_bound (), op2.upper_bound()))
	r = range_true (type);
      else
	r = range_false (type);
    }
  else
    {
      /* If ranges do not intersect, we know the range is not equal, otherwise
         we don;t really know anything for sure.  */
      r = range_intersect (op1, op2);
      if (r.undefined_p ())
	r = range_false (type);
      else
	r = range_true_and_false (type);
    }

  return r;
}

bool
operator_equal::op1_range (irange &r, tree type, const irange &lhs,
			    const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_FALSE:
        /* If the result is false, the only time we know anything is if OP2 is
	   a constant.  */
	if (wi::eq_p (op2.lower_bound(), op2.upper_bound()))
	  r = range_invert (op2);
	else
	  r.set_varying (type);
	break;

      case BRS_TRUE:
        /* If its true, the result is the same as OP2.  */
        r = op2;
	break;

      default:
        break;
    }
  return true;
}


bool
operator_equal::op2_range (irange &r, tree type, const irange &lhs,
			    const irange &op1) const
{
  return operator_equal::op1_range (r, type, lhs, op1);
}


/* Range operator for def = op1 != op2. */

class operator_not_equal : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
} op_not_equal;

/* Fold comparison of the 2 ranges.  */
irange
operator_not_equal::fold_range (tree type,
				const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  /* We can be sure the values are always equal or not if both ranges
     consist of a single value, and then compare them.  */
  if (wi::eq_p (op1.lower_bound (), op1.upper_bound ())
      && wi::eq_p (op2.lower_bound (), op2.upper_bound ()))
    {
      if (wi::ne_p (op1.lower_bound (), op2.upper_bound()))
	r = range_true (type);
      else
	r = range_false (type);
    }
  else
    {
      /* If ranges do not intersect, we know the range is not equal, otherwise
         we don;t really know anything for sure.  */
      r = range_intersect (op1, op2);
      if (r.undefined_p ())
	r = range_true (type);
      else
	r = range_true_and_false (type);
    }

  return r;
}

/* Calculate the range of op1 being == to VAL based on LHS.  */
bool
operator_not_equal::op1_range (irange &r, tree type, const irange &lhs,
				const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_TRUE:
        /* If the result is true, the only time we know anything is if OP2 is
	   a constant.  */
	if (wi::eq_p (op2.lower_bound(), op2.upper_bound()))
	  r = range_invert (op2);
	else
	  r.set_varying (type);
	break;

      case BRS_FALSE:
        /* If its true, the result is the same as OP2.  */
        r = op2;
	break;

      default:
        break;
    }
  return true;
}


bool
operator_not_equal::op2_range (irange &r, tree type, const irange &lhs,
				const irange &op1) const
{
  return operator_not_equal::op1_range (r, type, lhs, op1);
}


/* (X < VAL) produces the a range of [MIN, VAL - 1]  */
static void
build_lt (irange &r, tree type, const wide_int &val)
{
  wi::overflow_type ov;
  wide_int lim = wi::sub (val, 1, TYPE_SIGN (type), &ov);

  /* If val - 1 underflows, check is X < MIN, which is an empty range.  */
  if (ov)
    r.set_undefined ();
  else
    r = irange (type, min_limit (type), lim);
}

/* (X <= VAL) produces the a range of [MIN, VAL]  */
static void
build_le (irange &r, tree type, const wide_int &val)
{
  r = irange (type, min_limit (type), val);
}

/* (X > VAL) produces the a range of [VAL + 1, MAX]  */
static void
build_gt (irange &r, tree type, const wide_int &val)
{
  wi::overflow_type ov;
  wide_int lim = wi::add (val, 1, TYPE_SIGN (type), &ov);
  /* If val + 1 overflows, check is for X > MAX , which is an empty range.  */
  if (ov)
    r.set_undefined ();
  else
    r = irange (type, lim, max_limit (type));
}

/* (X >= val) produces the a range of [VAL, MAX]  */
static void
build_ge (irange &r, tree type, const wide_int &val)
{
  r = irange (type, val, max_limit (type));
}



class operator_lt :  public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
} op_lt;

irange
operator_lt::fold_range (tree type, const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  signop sign = TYPE_SIGN (op1.type ());
  gcc_checking_assert (sign == TYPE_SIGN (op2.type ()));

  if (wi::lt_p (op1.upper_bound (), op2.lower_bound (), sign))
    r = range_true (type);
  else
    if (!wi::lt_p (op1.lower_bound (), op2.upper_bound (), sign))
      r = range_false (type);
    else
      r = range_true_and_false (type);
  return r;
}


bool
operator_lt::op1_range (irange &r, tree type, const irange &lhs, const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_TRUE:
	build_lt (r, type, op2.upper_bound ());
	break;

      case BRS_FALSE:
	build_ge (r, type, op2.lower_bound ());
	break;

      default:
        break;
    }
  return true;
}


bool
operator_lt::op2_range (irange &r, tree type, const irange &lhs, const irange &op1) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_FALSE:
	build_le (r, type, op1.upper_bound ());
	break;

      case BRS_TRUE:
	build_gt (r, type, op1.lower_bound ());
	break;

      default:
        break;
    }
  return true;

}

class operator_le :  public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
  virtual bool op2_range (irange &r, tree type,
			  const irange &lhs, const irange &op1) const;
} op_le;

irange
operator_le::fold_range (tree type, const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  signop sign = TYPE_SIGN (op1.type ());
  gcc_checking_assert (sign == TYPE_SIGN (op2.type ()));

  if (wi::le_p (op1.upper_bound (), op2.lower_bound (), sign))
    r = range_true (type);
  else
    if (!wi::le_p (op1.lower_bound (), op2.upper_bound (), sign))
      r = range_false (type);
    else
      r = range_true_and_false (type);
  return r;
}

bool
operator_le::op1_range (irange &r, tree type, const irange &lhs, const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_TRUE:
	build_le (r, type, op2.upper_bound ());
	break;

      case BRS_FALSE:
	build_gt (r, type, op2.lower_bound ());
	break;

      default:
        break;
    }
  return true;
}


bool
operator_le::op2_range (irange &r, tree type, const irange &lhs, const irange &op1) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_FALSE:
	build_lt (r, type, op1.upper_bound ());
	break;

      case BRS_TRUE:
	build_ge (r, type, op1.lower_bound ());
	break;

      default:
        break;
    }
  return true;

}


class operator_gt :  public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
  virtual bool op2_range (irange &r, tree type,
			  const irange &lhs, const irange &op1) const;
} op_gt;

irange
operator_gt::fold_range (tree type, const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  signop sign = TYPE_SIGN (op1.type ());
  gcc_checking_assert (sign == TYPE_SIGN (op2.type ()));

  if (wi::gt_p (op1.lower_bound (), op2.upper_bound (), sign))
    r = range_true (type);
  else
    if (!wi::gt_p (op1.upper_bound (), op2.lower_bound (), sign))
      r = range_false (type);
    else
      r = range_true_and_false (type);

  return r;
}

bool
operator_gt::op1_range (irange &r, tree type, const irange &lhs, const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_TRUE:
	build_gt (r, type, op2.lower_bound ());
	break;

      case BRS_FALSE:
	build_le (r, type, op2.upper_bound ());
	break;

      default:
        break;
    }
  return true;
}


bool
operator_gt::op2_range (irange &r, tree type, const irange &lhs, const irange &op1) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_FALSE:
	build_ge (r, type, op1.lower_bound ());
	break;

      case BRS_TRUE:
	build_lt (r, type, op1.upper_bound ());
	break;

      default:
        break;
    }
  return true;

}


class operator_ge :  public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
  virtual bool op2_range (irange &r, tree type,
			  const irange &lhs, const irange &op1) const;
} op_ge;

irange
operator_ge::fold_range (tree type, const irange &op1, const irange &op2) const
{
  irange r;
  if (empty_range_check (r, op1, op2))
    return r;

  signop sign = TYPE_SIGN (op1.type ());
  gcc_checking_assert (sign == TYPE_SIGN (op2.type ()));

  if (wi::ge_p (op1.lower_bound (), op2.upper_bound (), sign))
    r = range_true (type);
  else
    if (!wi::ge_p (op1.upper_bound (), op2.lower_bound (), sign))
      r = range_false (type);
    else
      r = range_true_and_false (type);

  return r;
}

bool
operator_ge::op1_range (irange &r, tree type, const irange &lhs, const irange &op2) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_TRUE:
	build_ge (r, type, op2.lower_bound ());
	break;

      case BRS_FALSE:
	build_lt (r, type, op2.upper_bound ());
	break;

      default:
        break;
    }
  return true;
}


bool
operator_ge::op2_range (irange &r, tree type, const irange &lhs, const irange &op1) const
{
  switch (get_bool_state (r, lhs, type))
    {
      case BRS_FALSE:
	build_gt (r, type, op1.lower_bound ());
	break;

      case BRS_TRUE:
	build_le (r, type, op1.upper_bound ());
	break;

      default:
        break;
    }
  return true;

}

// ----------------------------------------------------------------------------

class operator_plus : public range_operator
{
public:
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_plus;


irange
operator_plus::wi_fold (tree type,
			const wide_int &lh_lb, const wide_int &lh_ub,
			const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  wi::overflow_type ov_lb, ov_ub;
  signop s = TYPE_SIGN (type);

  new_lb = wi::add (lh_lb, rh_lb, s, &ov_lb);
  new_ub = wi::add (lh_ub, rh_ub, s, &ov_ub);
  irange r;
  accumulate_range (r, type, new_lb, new_ub, ov_lb, ov_ub);
  return r;
}

/* Adjust irange to be in terms of op1.
   Given [range] = op1 + val,  op1 = [range] - val.  */
bool
operator_plus::op1_range (irange &r, tree type, const irange &lhs,
			  const irange &op2) const
{
  r = range_op_handler (MINUS_EXPR, type)->fold_range (type, lhs, op2);
  return true;
}

bool
operator_plus::op2_range (irange &r, tree type, const irange &lhs,
			  const irange &op1) const
{
  r = range_op_handler (MINUS_EXPR, type)->fold_range (type, lhs, op1);
  return true;
}

// ----------------------------------------------------------------------------

class operator_minus : public range_operator
{
public:
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_minus;


irange
operator_minus::wi_fold (tree type,
			 const wide_int &lh_lb, const wide_int &lh_ub,
			 const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  wi::overflow_type ov_lb, ov_ub;
  signop s = TYPE_SIGN (type);

  new_lb = wi::sub (lh_lb, rh_ub, s, &ov_lb);
  new_ub = wi::sub (lh_ub, rh_lb, s, &ov_ub);
  irange r;
  accumulate_range (r, type, new_lb, new_ub, ov_lb, ov_ub);
  return r;
}

/* Adjust irange to be in terms of op1.
   Given lhs = op1 - op2,  op1 = lhs + op2.  */
bool
operator_minus::op1_range (irange &r, tree type, const irange &lhs,
			    const irange &op2) const
{
  r = range_op_handler (PLUS_EXPR, type)->fold_range (type, lhs, op2);
  return true;
}

/* Adjust irange to be in terms of op2.
   Given lhs = op1 - op2,  -op2 = lhs - op1, therefore op2 = op1 - lhs.  */
bool
operator_minus::op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const
{
  r = fold_range (type, op1, lhs);
  return true;
}

// ----------------------------------------------------------------------------

class operator_min : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_min;


irange
operator_min::wi_fold (tree type,
		       const wide_int &lh_lb, const wide_int &lh_ub,
		       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub;
  signop s = TYPE_SIGN (type);

  new_lb = wi::min (lh_lb, rh_lb, s);
  new_ub = wi::min (lh_ub, rh_ub, s);
  irange r;
  accumulate_range (r, type, new_lb, new_ub);
  return r;
}

// ----------------------------------------------------------------------------

class operator_max : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_max;

irange
operator_max::wi_fold (tree type,
		       const wide_int &lh_lb, const wide_int &lh_ub,
		       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub;
  signop s = TYPE_SIGN (type);

  new_lb = wi::max (lh_lb, rh_lb, s);
  new_ub = wi::max (lh_ub, rh_ub, s);
  irange r;
  accumulate_range (r, type, new_lb, new_ub);
  return r;
}


// ----------------------------------------------------------------------------

class operator_mult : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_mult;


irange
operator_mult::wi_fold (tree type,
			const wide_int &lh_lb, const wide_int &lh_ub,
			const wide_int &rh_lb, const wide_int &rh_ub) const
{
  bool res;
  wide_int new_lb, new_ub;
  signop s = TYPE_SIGN (type);

  if (TYPE_OVERFLOW_UNDEFINED (type))
    res = wide_int_range_cross_product (new_lb, new_ub,
                                        MULT_EXPR, s,
                                        lh_lb, lh_ub, rh_lb, rh_ub, true);
  else
    res = wide_int_range_mult_wrapping (new_lb, new_ub,
                                         s, TYPE_PRECISION (type),
                                         lh_lb, lh_ub, rh_lb, rh_ub);
  if (res)
    {
      irange r;
      accumulate_possibly_reversed_range (r, type, new_lb, new_ub);
      return r;
    }
  return irange (type);
}


// ----------------------------------------------------------------------------

class operator_div : public range_operator
{
public:
  operator_div (enum tree_code c)  { code = c; }
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
private:
  enum tree_code code;
};

operator_div op_trunc_div (TRUNC_DIV_EXPR);
operator_div op_floor_div(FLOOR_DIV_EXPR);
operator_div op_round_div (ROUND_DIV_EXPR);
operator_div op_ceil_div (CEIL_DIV_EXPR);

irange
operator_div::wi_fold (tree type,
		       const wide_int &lh_lb, const wide_int &lh_ub,
		       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub;
  wide_int extra_min, extra_max;
  bool extra_range_p;

  /* If we know we will divide by zero, return an empty range,
     which will be interpreted as undefined.  */
  if (rh_lb == 0 && rh_ub == 0)
    return irange ();

  if (wide_int_range_div (new_lb, new_ub, code, TYPE_SIGN (type),
			  TYPE_PRECISION (type),
			  lh_lb, lh_ub,
			  rh_lb, rh_ub,
			  TYPE_OVERFLOW_UNDEFINED (type),
			  extra_range_p, extra_min, extra_max))
    {
      irange r;
      accumulate_range (r, type, new_lb, new_ub);
      if (extra_range_p)
	accumulate_range (r, type, extra_min, extra_max);
      return r;
    }
  return irange (type);
}


// ----------------------------------------------------------------------------

class operator_exact_divide : public operator_div
{
public:
  operator_exact_divide () : operator_div (EXACT_DIV_EXPR) { }
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;

} op_exact_div;


// Adjust irange to be in terms of op1.
bool
operator_exact_divide::op1_range (irange &r, tree type,
				   const irange &lhs,
				   const irange &op2) const
{
  tree offset;
  // [2, 4] = op1 / [3,3]   since its exact divide, no need to worry about
  // remainders in the endpoints, so op1 = [2,4] * [3,3] = [6,12].
  // We wont bother trying to enumerate all the in between stuff :-P
  // TRUE accuraacy is [6,6][9,9][12,12].  This is unlikely to matter most of
  // the time however.
  // If op2 is a multiple of 2, we would be able to set some non-zero bits.
  if (op2.singleton_p (&offset)
      && !integer_zerop (offset))
    {
      r = range_op_handler (MULT_EXPR, type)->fold_range (type, lhs, op2);
      return true;
    }
  return false;
}

// ----------------------------------------------------------------------------

class operator_lshift : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;

  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_lshift;



irange
operator_lshift::fold_range (tree type,
			     const irange &op1, const irange &op2) const
{
  // Check to see if the shift amount is undefined, and return if so.
  if (op2.undefined_p ())
    return irange ();

  if (wide_int_range_shift_undefined_p (TYPE_SIGN (op2.type ()),
					TYPE_PRECISION (type),
					op2.lower_bound (),
					op2.upper_bound ()))
    return irange (type);

  // Otherwise just invoke the normal fold routine.
  return range_operator::fold_range (type, op1, op2);
}

irange
operator_lshift::wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub;
  signop s = TYPE_SIGN (type);

  if (wide_int_range_lshift (new_lb, new_ub, s, TYPE_PRECISION (type),
			     lh_lb, lh_ub, rh_lb, rh_ub,
			     TYPE_OVERFLOW_UNDEFINED (type)))
    {
      irange r;
      accumulate_possibly_reversed_range (r, type, new_lb, new_ub);
      return r;
    }
  return irange (type);
}

// ----------------------------------------------------------------------------
//
class operator_rshift : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_rshift;


irange
operator_rshift::fold_range (tree type,
			     const irange &op1, const irange &op2) const
{
  // Check to see if the shift amount is undefined, and return if so.
  if (op2.undefined_p ())
    return irange ();

  if (wide_int_range_shift_undefined_p (TYPE_SIGN (op2.type ()),
					TYPE_PRECISION (type),
					op2.lower_bound (),
					op2.upper_bound ()))
    return irange (type);

  // Otherwise just invoke the normal fold routine.
  return range_operator::fold_range (type, op1, op2);
  
}

irange
operator_rshift::wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub;
  signop s = TYPE_SIGN (type);

  if (wide_int_range_multiplicative_op (new_lb, new_ub,
					RSHIFT_EXPR, s, TYPE_PRECISION (type),
					lh_lb, lh_ub, rh_lb, rh_ub,
					TYPE_OVERFLOW_UNDEFINED (type)))
    {
      irange r;
      accumulate_possibly_reversed_range (r, type, new_lb, new_ub);
      return r;
    }
  return irange (type);
}

// ----------------------------------------------------------------------------


class operator_cast: public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;

} op_convert;


/* Return the range of lh converted to the type of rh:
   r = (type_of(rh)) lh.  */

irange
operator_cast::fold_range (tree type ATTRIBUTE_UNUSED,
			   const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  if (lh.type () != rh.type ())
    {
      /* Handle conversion so they become the same type.  */
      r = lh;
      r.cast (rh.type ());
      r.intersect (rh);
    }
  else
    /* If they are the same type, the result should be the intersection of
       the two ranges.  */
    r = range_intersect (lh, rh);
  return r;
}

bool
operator_cast::op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const
{
  tree lhs_type = lhs.type ();
  gcc_checking_assert (types_compatible_p (op2.type(), type));

  /* If the precision of the LHS is smaller than the precision of the RHS,
     then there would be truncation of the value on the RHS, and so we can tell
     nothing about it.  */
  if (TYPE_PRECISION (lhs_type) < TYPE_PRECISION (type))
    {
      /* If we've been passed an actual value for the RHS rather than the type
	 see if it fits the LHS, and if so, then we can allow it.  */
      r = op2;
      r.cast (lhs_type);
      r.cast (type);
      if (r == op2)
        {
	  /* We know the value of the RHS fits in the LHS type, so convert the
	     left hand side and remove any values that arent in OP2.  */
	  r = lhs;
	  r.cast (type);
	  r.intersect (op2);
	  return true;
	}
      /* Special case if the LHS is a boolean.  A 0 means the RHS is zero,
	 and a 1 means the RHS is non-zero.  */
      if (TREE_CODE (lhs_type) == BOOLEAN_TYPE)
	{
	  /* If the LHS is unknown, the result is whatever op2 already is.  */
	  if (!lhs.singleton_p ())
	    {
	      r = op2;
	      return true;
	    }
	  /* Boolean casts are weird in GCC. It's actually an implied mask with
	      0x01, so all that is known is whether the rightmost bit is 0 or 1,
	      which implies the only value *not* in the RHS is 0 or -1.  */
	  unsigned prec = TYPE_PRECISION (type);
	  if (lhs.zero_p ())
	    r = irange (VR_ANTI_RANGE, type,
			wi::minus_one (prec), wi::minus_one (prec));
	  else
	    r = irange (VR_ANTI_RANGE, type,
			wi::zero (prec), wi::zero (prec));
	  /* And intersect it with what we know about op2.  */
	  r.intersect (op2);
	}
      else
	/* Otherwise we'll have to assume it's whatever we know about op2.  */
	r = op2;
      return true;
    }

  /* If the LHS precision is greater than the rhs precision, the LHS range
     is resticted to the range of the RHS by this assignment.  */
  if (TYPE_PRECISION (lhs_type) > TYPE_PRECISION (type))
    {
      /* Cast the range of the RHS to the type of the LHS. */
      irange op_type (type);
      op_type.cast (lhs_type);

      /* Intersect this with the LHS range will produce the RHS range.  */
      r = range_intersect (lhs, op_type);
    }
  else
    r = lhs;

  /* Cast the calculated range to the type of the RHS.  */
  r.cast (type);
  return true;
}

/*  ----------------------------------------------------------------------  */

// Bitwise and logical ops.

class operator_logical_and : public range_operator
{
public:
  virtual irange fold_range (tree type, const irange &lh, const irange &rh) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
} op_logical_and;


irange
operator_logical_and::fold_range (tree type,
				  const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  // 0 && anything is 0
  if ((wi::eq_p (lh.lower_bound (), 0) && wi::eq_p (lh.upper_bound (), 0))
      || (wi::eq_p (lh.lower_bound (), 0) && wi::eq_p (rh.upper_bound (), 0)))
    return range_false (type);

  // To reach this point, there must be a logical 1 on each side, and the only
  // remaining question is whether there is a zero or not.

  if (lh.contains_p (build_zero_cst (lh.type ()))
      || rh.contains_p (build_zero_cst (rh.type ())))
    return range_true_and_false (type);

  return range_true (type);
}



bool
operator_logical_and::op1_range (irange &r, tree type, const irange &lhs,
				 const irange &op2 ATTRIBUTE_UNUSED) const
{
   switch (get_bool_state (r, lhs, type))
     {
       /* A true result means both sides of the AND must be true.  */
       case BRS_TRUE:
         r = range_true (type);
	 break;
       /* Any other result means only one side has to be false, the other
	  side can be anything. SO we cant be sure of any result here.  */
      default:
	r = range_true_and_false (type);
	break;
    }
  return true;
}

bool
operator_logical_and::op2_range (irange &r, tree type,
				 const irange &lhs, const irange &op1) const
{
  return operator_logical_and::op1_range (r, type, lhs, op1);
}

class operator_bitwise_and : public range_operator
{
public:
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			  const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			  const irange &op1) const;
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_bitwise_and;


irange
operator_bitwise_and::wi_fold (tree type,
			       const wide_int &lh_lb, const wide_int &lh_ub,
			       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  signop s = TYPE_SIGN (type);

  wide_int may_be_nonzero_lh, must_be_nonzero_lh;
  wide_int may_be_nonzero_rh, must_be_nonzero_rh;
  wide_int_range_set_zero_nonzero_bits (s, lh_lb, lh_ub,
					may_be_nonzero_lh,
					must_be_nonzero_lh);
  wide_int_range_set_zero_nonzero_bits (s, rh_lb, rh_ub,
					may_be_nonzero_rh,
					must_be_nonzero_rh);
  if (wide_int_range_bit_and (new_lb, new_ub, s, TYPE_PRECISION (type),
			      lh_lb, lh_ub,
			      rh_lb, rh_ub,
			      must_be_nonzero_lh,
			      may_be_nonzero_lh,
			      must_be_nonzero_rh,
			      may_be_nonzero_rh))
    {
      // For AND, calculate each subrange separately, and then union
      // the results.
      irange tmp;
      accumulate_range (tmp, type, new_lb, new_ub);
      return tmp;
    }
  return irange (type);
}

bool
operator_bitwise_and::op1_range (irange &r, tree type,
				 const irange &lhs, const irange &op2) const
{
  /* If this is really a logical wi_fold, call that.  */
  if (types_compatible_p (type, boolean_type_node))
    return op_logical_and.op1_range (r, type, lhs, op2);

  /* For now do nothing with bitwise AND of iranges, just return the type. */
  r.set_varying (type);
  return true;
}

bool
operator_bitwise_and::op2_range (irange &r, tree type, const irange &lhs,
				  const irange &op1) const
{
  return operator_bitwise_and::op1_range (r, type, lhs, op1);
}


class operator_logical_or : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &lh, const irange &rh) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
  virtual bool op2_range (irange &r, tree type,
			  const irange &lhs, const irange &op1) const;
} op_logical_or;


irange
operator_logical_or::fold_range (tree type ATTRIBUTE_UNUSED,
				 const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  return range_union (lh, rh);
}

bool
operator_logical_or::op1_range (irange &r, tree type, const irange &lhs,
				const irange &op2 ATTRIBUTE_UNUSED) const
{
   switch (get_bool_state (r, lhs, type))
     {
       /* A false result means both sides of the OR must be false.  */
       case BRS_FALSE:
         r = range_false (type);
	 break;
       /* Any other result means only one side has to be true, the other
	  side can be anything. SO we cant be sure of any result here.  */
      default:
	r = range_true_and_false (type);
	break;
    }
  return true;
}

bool
operator_logical_or::op2_range (irange &r, tree type, const irange &lhs,
				  const irange &op1) const
{
  return operator_logical_or::op1_range (r, type, lhs, op1);
}

class operator_bitwise_or : public range_operator
{
public:
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
  virtual bool op2_range (irange &r, tree type, const irange &lhs,
			   const irange &op1) const;
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_bitwise_or;


irange
operator_bitwise_or::wi_fold (tree type,
			      const wide_int &lh_lb, const wide_int &lh_ub,
			      const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  signop s = TYPE_SIGN (type);

  wide_int may_be_nonzero_lh, must_be_nonzero_lh;
  wide_int may_be_nonzero_rh, must_be_nonzero_rh;
  wide_int_range_set_zero_nonzero_bits (s, lh_lb, lh_ub,
					may_be_nonzero_lh,
					must_be_nonzero_lh);
  wide_int_range_set_zero_nonzero_bits (s, rh_lb, rh_ub,
					may_be_nonzero_rh,
					must_be_nonzero_rh);
  if (wide_int_range_bit_ior (new_lb, new_ub, s,
			      lh_lb, lh_ub,
			      rh_lb, rh_ub,
			      must_be_nonzero_lh,
			      may_be_nonzero_lh,
			      must_be_nonzero_rh,
			      may_be_nonzero_rh))
    {
      irange r;
      accumulate_range (r, type, new_lb, new_ub);
      return r;
    }
  return irange (type);
}

bool
operator_bitwise_or::op1_range (irange &r, tree type,
				const irange &lhs, const irange &op2) const
{
  /* If this is really a logical wi_fold, call that.  */
  if (types_compatible_p (type, boolean_type_node))
    return op_logical_or.op1_range (r, type, lhs, op2);

  /* For now do nothing with bitwise OR of iranges, just return the type. */
  r.set_varying (type);
  return true;
}

bool
operator_bitwise_or::op2_range (irange &r, tree type,
				const irange &lhs, const irange &op1) const
{
  return operator_bitwise_or::op1_range (r, type, lhs, op1);
}

class operator_bitwise_xor : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_bitwise_xor;


irange
operator_bitwise_xor::wi_fold (tree type,
			       const wide_int &lh_lb, const wide_int &lh_ub,
			       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  signop s = TYPE_SIGN (type);

  wide_int may_be_nonzero_lh, must_be_nonzero_lh;
  wide_int may_be_nonzero_rh, must_be_nonzero_rh;
  wide_int_range_set_zero_nonzero_bits (s, lh_lb, lh_ub,
					may_be_nonzero_lh,
					must_be_nonzero_lh);
  wide_int_range_set_zero_nonzero_bits (s, rh_lb, rh_ub,
					may_be_nonzero_rh,
					must_be_nonzero_rh);
  if (wide_int_range_bit_xor (new_lb, new_ub, s, TYPE_PRECISION (type),
			      must_be_nonzero_lh,
			      may_be_nonzero_lh,
			      must_be_nonzero_rh,
			      may_be_nonzero_rh))
    {
      irange r;
      accumulate_range (r, type, new_lb, new_ub);
      return r;
    }
  return irange (type);
}


class operator_trunc_mod : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_trunc_mod;


irange
operator_trunc_mod::wi_fold (tree type,
			     const wide_int &lh_lb, const wide_int &lh_ub,
			     const wide_int &rh_lb, const wide_int &rh_ub) const
{
  wide_int new_lb, new_ub, tmp;
  signop s = TYPE_SIGN (type);

  /* Mod 0 is undefined.  Return undefined.  */
  if (wide_int_range_zero_p (rh_lb, rh_ub, TYPE_PRECISION (type)))
    return irange ();

  wide_int_range_trunc_mod (new_lb, new_ub, s, TYPE_PRECISION (type),
			    lh_lb, lh_ub, rh_lb, rh_ub);
  irange r;
  accumulate_range (r, type, new_lb, new_ub);
  return r;
}


class operator_logical_not : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &lh, const irange &rh) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
} op_logical_not;


/* Folding a logical NOT, oddly enough, involves doing nothing on the
   forward pass thru.  During the initial walk backwards, the logical NOT
   reversed the desired outcome on the way back, so on the way forward all
   we do is pass the range forward.
	b_2 = x_1 < 20
	b_3 = !b_2
	if (b_3)
    to determine the TRUE branch, walking  backward
         if (b_3)		if ([1,1])
         b_3 = !b_2		[1,1] = ![0,0]
	 b_2 = x_1 < 20		[0,0] = x_1 < 20,   false, so x_1 == [20, 255]
     which is the result we are looking for.. so.. pass it thru.  */

irange
operator_logical_not::fold_range (tree type, const irange &lh,
				  const irange &rh ATTRIBUTE_UNUSED) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  if (lh.varying_p () || lh.undefined_p ())
    r = lh;
  else
    r = range_invert (lh);
  gcc_checking_assert (lh.type() == type);
  return r;
}

bool
operator_logical_not::op1_range (irange &r, tree type ATTRIBUTE_UNUSED,
				 const irange &lhs,
				 const irange &op2 ATTRIBUTE_UNUSED) const
{
  if (lhs.varying_p () || lhs.undefined_p ())
    r = lhs;
  else
    r = range_invert (lhs);
  return true;
}


class operator_bitwise_not : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &lh, const irange &rh) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
} op_bitwise_not;

irange
operator_bitwise_not::fold_range (tree type,
				  const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  // ~X is simply -1 - X.
  irange minusone (type,
		   wi::minus_one (TYPE_PRECISION (type)),
		   wi::minus_one (TYPE_PRECISION (type)));
  r = range_op_handler (MINUS_EXPR, type)->fold_range (type, minusone, lh);
  return r;
}

bool
operator_bitwise_not::op1_range (irange &r, tree type,
				 const irange &lhs, const irange &op2) const
{
  // ~X is -1 - X and since bitwise NOT is involutary...do it again.
  r = fold_range (type, lhs, op2);
  return true;
}


/*  ----------------------------------------------------------------------  */


class operator_cst : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
} op_integer_cst;


irange
operator_cst::fold_range (tree type ATTRIBUTE_UNUSED,
			  const irange &lh,
			  const irange &rh ATTRIBUTE_UNUSED) const
{
  return lh;
}

/*  ----------------------------------------------------------------------  */


class operator_identity : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			   const irange &op2) const;
} op_identity;

irange
operator_identity::fold_range (tree type ATTRIBUTE_UNUSED,
			       const irange &lh,
			       const irange &rh ATTRIBUTE_UNUSED) const
{
  return lh;
}

bool
operator_identity::op1_range (irange &r, tree type ATTRIBUTE_UNUSED,
			      const irange &lhs,
			      const irange &op2 ATTRIBUTE_UNUSED) const
{
  r = lhs;
  return true;
}


/*  ----------------------------------------------------------------------  */

class operator_abs : public range_operator
{
 public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
  virtual bool op1_range (irange &r, tree type,
			  const irange &lhs, const irange &op2) const;
} op_abs;


irange
operator_abs::wi_fold (tree type,
		       const wide_int &lh_lb, const wide_int &lh_ub,
		       const wide_int &rh_lb ATTRIBUTE_UNUSED,
		       const wide_int &rh_ub ATTRIBUTE_UNUSED) const
{
  wide_int new_lb, new_ub, tmp;

  if (wide_int_range_abs (new_lb, new_ub,
			  TYPE_SIGN (type),
			  TYPE_PRECISION (type),
			  lh_lb, lh_ub,
			  TYPE_OVERFLOW_UNDEFINED (type)))
    return irange (type, new_lb, new_ub);
  return irange (type);
}


bool
operator_abs::op1_range (irange &r, tree type,
			 const irange &lhs, const irange &op2) const
{
  if (empty_range_check (r, lhs, op2))
    return true;
  if (TYPE_UNSIGNED (type))
    {
      r = lhs;
      return true;
    }
  // Start with the positives because negatives are an impossible result.
  irange positives = range_positives (type);
  positives.intersect (lhs);
  r = positives;
  // Then add the negative of each pair:
  // ABS(op1) = [5,20] would yield op1 => [-20,-5][5,20].
  for (unsigned i = 0; i < positives.num_pairs (); ++i)
    r.union_ (irange (type,
		      -positives.upper_bound (i),
		      -positives.lower_bound (i)));
  return true;
}

class operator_absu : public range_operator
{
 public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_absu;

irange
operator_absu::wi_fold (tree type,
			const wide_int &lh_lb, const wide_int &lh_ub,
			const wide_int &rh_lb ATTRIBUTE_UNUSED,
			const wide_int &rh_ub ATTRIBUTE_UNUSED) const
{
  wide_int new_lb, new_ub;
  
  /* Pass through VR0 the easy cases.  */
  if (wi::ges_p (lh_lb, 0))
    {
      new_lb = lh_lb;
      new_ub = lh_ub;
    }
  else
    {
      new_lb = wi::abs (lh_lb);
      new_ub = wi::abs (lh_ub);

      /* If the range contains zero then we know that the minimum value in the
	 range will be zero.  */
      if (wi::ges_p (lh_ub, 0))
	{
	  if (wi::gtu_p (new_lb, new_ub))
	    new_ub = new_lb;
	  new_lb = wi::zero (TYPE_PRECISION (type));
	}
      else
	/* Otherwise, swap MIN and MAX.  */
	std::swap (new_lb, new_ub);
    }

//  r.union_ (irange (unsigned_type_for (type), new_lb, new_ub));
  gcc_checking_assert (TYPE_UNSIGNED (type));
  return irange (type, new_lb, new_ub);
}

class operator_negate : public range_operator
{
 public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			  const irange &op2) const;
} op_negate;

/* Return the negated range of lh with the type of rh.  */

irange
operator_negate::fold_range (tree type,
			     const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;
  // -X is simply 0 - X.
  return
    range_op_handler (MINUS_EXPR, type)->fold_range (type,
						     range_zero (type), lh);
}

bool
operator_negate::op1_range (irange &r, tree type,
			    const irange &lhs, const irange &op2) const
{
  // NEGATE is involutory.
  r = fold_range (type, lhs, op2);
  return true;
}

class operator_addr_expr : public range_operator
{
public:
  virtual irange fold_range (tree type,
			     const irange &op1, const irange &op2) const;
  virtual bool op1_range (irange &r, tree type, const irange &lhs,
			  const irange &op2) const;
} op_addr;

irange
operator_addr_expr::fold_range (tree type,
				const irange &lh, const irange &rh) const
{
  irange r;
  if (empty_range_check (r, lh, rh))
    return r;

  // Return a non-null pointer of the LHS type (passed in op2)
  if (lh.zero_p ())
    return range_zero (type);
  if (!lh.contains_p (build_zero_cst (lh.type ())))
    return range_nonzero (type);
  return irange (type);
}

// The same functionality for fold() applies to op1_range...
// effectively copying the non-nullness.
bool
operator_addr_expr::op1_range (irange &r, tree type, const irange &lhs,
			       const irange &op2) const
{
  r = operator_addr_expr::fold_range (type, lhs, op2);
  return true;
}

// ----------------------------------------------------------------------

// ---------------------------------------------------------------------------

class pointer_plus_operator : public range_operator
{
public:
  virtual irange wi_fold (tree type,
                          const wide_int &lh_lb, const wide_int &lh_ub,
                          const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_pointer_plus;


irange 
pointer_plus_operator::wi_fold (tree type,
				const wide_int &lh_lb, const wide_int &lh_ub,
				const wide_int &rh_lb, const wide_int &rh_ub) const
{
  unsigned prec = lh_lb.get_precision ();
  signop sign = TYPE_SIGN (type);
  /* For pointer types, we are really only interested in asserting
     whether the expression evaluates to non-NULL.

     With -fno-delete-null-pointer-checks we need to be more
     conservative.  As some object might reside at address 0,
     then some offset could be added to it and the same offset
     subtracted again and the result would be NULL.
     E.g.
     static int a[12]; where &a[0] is NULL and
     ptr = &a[6];
     ptr -= 6;
     ptr will be NULL here, even when there is POINTER_PLUS_EXPR
     where the first range doesn't include zero and the second one
     doesn't either.  As the second operand is sizetype (unsigned),
     consider all ranges where the MSB could be set as possible
     subtractions where the result might be NULL.  */
  if ((!wide_int_range_includes_zero_p (lh_lb, lh_ub, sign)
       || !wide_int_range_includes_zero_p (rh_lb, rh_ub, sign))
      && !TYPE_OVERFLOW_WRAPS (type)
      && (flag_delete_null_pointer_checks
	  || !wi::sign_mask (rh_ub)))
    return range_nonzero (type);
  if (wide_int_range_zero_p (lh_lb, lh_ub, prec)
      && wide_int_range_zero_p (rh_lb, rh_ub, prec))
    return range_zero (type);
  return irange (type);
}

// ---------------------------------------------------------------------------

class pointer_min_max_operator : public range_operator
{
public:
  virtual irange wi_fold (tree type,
                          const wide_int &lh_lb, const wide_int &lh_ub,
                          const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_ptr_min_max;


irange 
pointer_min_max_operator::wi_fold (tree type,
				   const wide_int &lh_lb,
				   const wide_int &lh_ub,
				   const wide_int &rh_lb,
				   const wide_int &rh_ub) const
{
  /* For MIN/MAX expressions with pointers, we only care about
   nullness, if both are non null, then the result is nonnull.
   If both are null, then the result is null. Otherwise they
   are varying.  */

  unsigned prec = lh_lb.get_precision ();
  signop sign = TYPE_SIGN (type);

  if (!wide_int_range_includes_zero_p (lh_lb, lh_ub, sign)
      && !wide_int_range_includes_zero_p (rh_lb, rh_ub, sign))
    return range_nonzero (type);
  if (wide_int_range_zero_p (lh_lb, lh_ub, prec)
      && wide_int_range_zero_p (rh_lb, rh_ub, prec))
    return range_zero (type);
  return irange (type);
}

// ---------------------------------------------------------------------------
//
class pointer_and_operator : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_pointer_and;


irange
pointer_and_operator::wi_fold (tree type,
			       const wide_int &lh_lb, const wide_int &lh_ub,
			       const wide_int &rh_lb, const wide_int &rh_ub) const
{
  unsigned prec = lh_lb.get_precision ();
  signop sign = TYPE_SIGN (type);

  /* For pointer types, we are really only interested in asserting
     whether the expression evaluates to non-NULL.  */

  if (!wide_int_range_includes_zero_p (lh_lb, lh_ub, sign)
      && !wide_int_range_includes_zero_p (rh_lb, rh_ub, sign))
    return range_nonzero (type);
  if (wide_int_range_zero_p (lh_lb, lh_ub, prec)
      || wide_int_range_zero_p (lh_lb, lh_ub, prec))
    return range_zero (type);

  return irange (type);
}


// -------------------------------------------------------------------------


class pointer_or_operator : public range_operator
{
public:
  virtual irange wi_fold (tree type,
			  const wide_int &lh_lb, const wide_int &lh_ub,
			  const wide_int &rh_lb, const wide_int &rh_ub) const;
} op_pointer_or;


irange 
pointer_or_operator::wi_fold (tree type,
			      const wide_int &lh_lb, const wide_int &lh_ub,
			      const wide_int &rh_lb, const wide_int &rh_ub) const
{
  unsigned prec = lh_lb.get_precision ();
  signop sign = TYPE_SIGN (type);

  /* For pointer types, we are really only interested in asserting
     whether the expression evaluates to non-NULL.  */

  if (!wide_int_range_includes_zero_p (lh_lb, lh_ub, sign)
      && !wide_int_range_includes_zero_p (rh_lb, rh_ub, sign))
    return range_nonzero (type);
  if (wide_int_range_zero_p (lh_lb, lh_ub, prec)
      && wide_int_range_zero_p (rh_lb, rh_ub, prec))
    return range_zero (type);
  return irange (type);
}


// -------------------------------------------------------------------------

// This implements the range operator tables as local objects in this file.

class range_op_table
{
public:
  inline range_operator *operator[] (enum tree_code code);
protected:
  void set (enum tree_code code, range_operator &op);
private:
  range_operator *m_range_tree[MAX_TREE_CODES];
};

// Return a pointer to the range_operator instance, if there is one,
// associated with tree_code CODE.

range_operator *
range_op_table::operator[] (enum tree_code code)
{
  gcc_assert (code > 0 && code < MAX_TREE_CODES);
  return m_range_tree[code];
}

// Add OP to the handler table for CODE.

void
range_op_table::set (enum tree_code code, range_operator &op)
{
  gcc_checking_assert (m_range_tree[code] == NULL);
  m_range_tree[code] = &op;
}

// Instantiate a range op table for integral operations.
class integral_table : public range_op_table
{
public:
  integral_table ();
} integral_tree_table;

integral_table::integral_table ()
{
  set (EQ_EXPR, op_equal);
  set (NE_EXPR, op_not_equal);
  set (LT_EXPR, op_lt);
  set (LE_EXPR, op_le);
  set (GT_EXPR, op_gt);
  set (GE_EXPR, op_ge);
  set (PLUS_EXPR, op_plus);
  set (MINUS_EXPR, op_minus);
  set (MIN_EXPR, op_min);
  set (MAX_EXPR, op_max);
  set (MULT_EXPR, op_mult);
  set (TRUNC_DIV_EXPR, op_trunc_div);
  set (FLOOR_DIV_EXPR, op_floor_div);
  set (ROUND_DIV_EXPR, op_round_div);
  set (CEIL_DIV_EXPR, op_ceil_div);
  set (EXACT_DIV_EXPR, op_exact_div);
  set (LSHIFT_EXPR, op_lshift);
  set (RSHIFT_EXPR, op_rshift);
  set (NOP_EXPR, op_convert);
  set (CONVERT_EXPR, op_convert);
  set (TRUTH_AND_EXPR, op_logical_and);
  set (BIT_AND_EXPR, op_bitwise_and);
  set (TRUTH_OR_EXPR, op_logical_or);
  set (BIT_IOR_EXPR, op_bitwise_or);
  set (BIT_XOR_EXPR, op_bitwise_xor);
  set (TRUNC_MOD_EXPR, op_trunc_mod);
  set (TRUTH_NOT_EXPR, op_logical_not);
  set (BIT_NOT_EXPR, op_bitwise_not);
  set (INTEGER_CST, op_integer_cst);
  set (SSA_NAME, op_identity);
  set (PAREN_EXPR, op_identity);
  set (OBJ_TYPE_REF, op_identity);
  set (ABS_EXPR, op_abs);
  set (ABSU_EXPR, op_absu);
  set (NEGATE_EXPR, op_negate);
  set (ADDR_EXPR, op_addr);
}


// Instantiate a range op table for pointer operations.
class pointer_table : public range_op_table
{
public:
  pointer_table ();
} pointer_tree_table;

pointer_table::pointer_table ()
{
  set (BIT_AND_EXPR, op_pointer_and);
  set (BIT_IOR_EXPR, op_pointer_or);
  set (MIN_EXPR, op_ptr_min_max);
  set (MAX_EXPR, op_ptr_min_max);
  set (POINTER_PLUS_EXPR, op_pointer_plus);

  set (EQ_EXPR, op_equal);
  set (NE_EXPR, op_not_equal);
  set (LT_EXPR, op_lt);
  set (LE_EXPR, op_le);
  set (GT_EXPR, op_gt);
  set (GE_EXPR, op_ge);
  set (SSA_NAME, op_identity);
  set (ADDR_EXPR, op_addr);
  set (NOP_EXPR, op_convert);
  set (CONVERT_EXPR, op_convert);

  set (BIT_NOT_EXPR, op_bitwise_not);
  set (BIT_XOR_EXPR, op_bitwise_xor);
}




/* The tables are hidden and accessed via a simple extern function.  */

range_operator *
range_op_handler (enum tree_code code, tree type)
{
  // First check if there is apointer specialization.
  if (POINTER_TYPE_P (type))
    return pointer_tree_table[code];
  return integral_tree_table[code];
}






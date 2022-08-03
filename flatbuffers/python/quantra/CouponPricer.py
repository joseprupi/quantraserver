# automatically generated by the FlatBuffers compiler, do not modify

# namespace: quantra

import flatbuffers
from flatbuffers.compat import import_numpy
np = import_numpy()

class CouponPricer(object):
    __slots__ = ['_tab']

    @classmethod
    def GetRootAs(cls, buf, offset=0):
        n = flatbuffers.encode.Get(flatbuffers.packer.uoffset, buf, offset)
        x = CouponPricer()
        x.Init(buf, n + offset)
        return x

    @classmethod
    def GetRootAsCouponPricer(cls, buf, offset=0):
        """This method is deprecated. Please switch to GetRootAs."""
        return cls.GetRootAs(buf, offset)
    # CouponPricer
    def Init(self, buf, pos):
        self._tab = flatbuffers.table.Table(buf, pos)

    # CouponPricer
    def Id(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(4))
        if o != 0:
            return self._tab.String(o + self._tab.Pos)
        return None

    # CouponPricer
    def PricerType(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(6))
        if o != 0:
            return self._tab.Get(flatbuffers.number_types.Uint8Flags, o + self._tab.Pos)
        return 0

    # CouponPricer
    def Pricer(self):
        o = flatbuffers.number_types.UOffsetTFlags.py_type(self._tab.Offset(8))
        if o != 0:
            from flatbuffers.table import Table
            obj = Table(bytearray(), 0)
            self._tab.Union(obj, o)
            return obj
        return None

def Start(builder): builder.StartObject(3)
def CouponPricerStart(builder):
    """This method is deprecated. Please switch to Start."""
    return Start(builder)
def AddId(builder, id): builder.PrependUOffsetTRelativeSlot(0, flatbuffers.number_types.UOffsetTFlags.py_type(id), 0)
def CouponPricerAddId(builder, id):
    """This method is deprecated. Please switch to AddId."""
    return AddId(builder, id)
def AddPricerType(builder, pricerType): builder.PrependUint8Slot(1, pricerType, 0)
def CouponPricerAddPricerType(builder, pricerType):
    """This method is deprecated. Please switch to AddPricerType."""
    return AddPricerType(builder, pricerType)
def AddPricer(builder, pricer): builder.PrependUOffsetTRelativeSlot(2, flatbuffers.number_types.UOffsetTFlags.py_type(pricer), 0)
def CouponPricerAddPricer(builder, pricer):
    """This method is deprecated. Please switch to AddPricer."""
    return AddPricer(builder, pricer)
def End(builder): return builder.EndObject()
def CouponPricerEnd(builder):
    """This method is deprecated. Please switch to End."""
    return End(builder)
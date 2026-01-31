import grpc
import flatbuffers
from quantra.PriceFixedRateBondRequest import PriceFixedRateBondRequestT
from quantra.PriceFixedRateBondResponse import PriceFixedRateBondResponse

class Client:
    def __init__(self, target="localhost:50051"):
        self.channel = grpc.insecure_channel(target)
        
    def price_fixed_rate_bonds(self, request_object: PriceFixedRateBondRequestT):
        """
        Takes a high-level python object (PriceFixedRateBondRequestT),
        serializes it, sends it to server, and returns the response READER.
        """
        return self._call(
            method="/quantra.QuantraServer/PriceFixedRateBond",
            request_obj=request_object,
            response_class=PriceFixedRateBondResponse
        )

    def _call(self, method, request_obj, response_class):
        # 1. Serialize: Create a builder and Pack the object
        builder = flatbuffers.Builder(1024)
        offset = request_obj.Pack(builder)
        builder.Finish(offset)
        request_bytes = bytes(builder.Output())

        # 2. Transmit: Send raw bytes over gRPC
        unary_call = self.channel.unary_unary(
            method,
            request_serializer=lambda x: x,
            response_deserializer=lambda x: x,
        )
        
        try:
            response_bytes = unary_call(request_bytes)
        except grpc.RpcError as e:
            print(f"RPC Error: {e.details()}")
            raise

        # 3. Deserialize: Get the Reader (Do not Unpack)
        # This returns the raw Table accessor which is always available
        return response_class.GetRootAs(response_bytes, 0)